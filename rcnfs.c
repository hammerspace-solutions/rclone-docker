/* vim: set ts=4 sw=4 expandtab: */
/*
 * rcnfs.c - rclone NFS service helper
 *
 * This program is a helper utility to set up and manage an NFS service using rclone.
 * It performs the following tasks:
 * 1. Starts the rclone NFS service by:
 *    - Finding a free port.
 *    - Creating a fake root handle.
 *    - Spawning the mountd daemon process.
 *    - Spawning the rclone server process.
 *    - Registering the rclone NFS program with rpcbind.
 *    - Exporting the NFS filesystem using exportfs.
 * 2. Enters an infinite loop to keep the service running until interrupted.
 * 3. On receiving a termination signal, the program cleans up by:
 *    - Unexporting the NFS filesystem.
 *    - unregistering the rclone NFS program.
 *    - Terminating the rclone and mountd processes.
 *
 * Dependencies:
 * -------------
 * - rclone
 * - mountd
 * - rpcbind
 * - exportfs
 * - libtirpc (including headers)
 *
 * Build:
 * ------
 * gcc [-g -O0] -Wall -I/usr/include/tirpc/ -o rcnfs rcnfs.c -ltirpc
 *
 * Notes:
 * -----
 * - rclone configuration must be set up for the root user.
 * - For now, the root of the remote is exported; there is no way to define a subdirectory.
 * - This program requires root privileges because exportfs needs to modify the NFS exports table.
 * - any other nfs server (e.g. nfskd) should be stopped. 
 *
 * Usage:
 * -----
 * rcnfs [-a ANVIL_IP] rclone_config
 *
 * Example: ./rcnfs -a 10.200.104.39 S3:hs-rclone
 *
 * Example mount command from a client:
 * ------------------------------------
 * sudo mount -t nfs -o vers=3 <server_ip>:/export /mnt
 *
 * Note: the kernel exportfs path is /export (not /) so the underlying
 * filesystem for that path supports NFS export (overlay/overlayfs does
 * not). The container/runtime is responsible for ensuring /export exists
 * on a supported filesystem, typically by mounting a tmpfs there before
 * rcnfs runs. rclone serves the remote regardless of the export path.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <getopt.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/mount.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <rpc/rpc.h>
#include <rpc/pmap_clnt.h>

#define CACHE_DIR           "/var/cache/rclone"
#define ROOT_HANDLE_FILE    "0100010000000000"
#define PROC_NFSD           "/proc/fs/nfsd"

struct Bucket {
    char name[256];
    struct Bucket *next;
};

const unsigned long NFS_PROGRAM = 100003;
const unsigned long NFS_VERSION = 3;

static int mkdirs(const char *path); 
static int fake_root_handle();
/* static int free_port(); */
static int is_port_free(int port);
static int mount_nfsd();
static int spawn_mountd(); 
static int kill_mountd();
static int spawn_rclone();
static int kill_rclone();
static int exportfs();
static int unexportfs();
static int register_rclone();
static int unregister_rclone(); 
static void free_buckets(); 
static void startup();
static void teardown();
static void trap(int signum);
static void usage();

static pid_t rclone_pid = 0;
static pid_t mountd_pid = 0;
static char anvil[256] = "";
static int port = 0;
static char remote[256] = "";
static struct Bucket *buckets = 0;

/**
 * mkdirs - Create directories recursively
 * @path: The directory path to create
 * 
 * This function creates the specified directory path recursively. If the directories 
 * already exist, it does nothing. If any directory in the path cannot be created, 
 * it returns -1 and prints an error message.
 * 
 * Return: 0 on success, -1 on failure
 */
int
mkdirs(const char *path) {
    char tmp[4096];
    char *p = NULL;
    size_t len;

    snprintf(tmp, sizeof(tmp), "%s", path);
    len = strlen(tmp);

    if (tmp[len - 1] == '/') {
        tmp[len - 1] = '\0';
    }

    for (p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(tmp, 0755) != 0 && errno != EEXIST) {
                perror("mkdir");
                return -1;
            }
            *p = '/';
        }
    }

    if (mkdir(tmp, 0755) != 0 && errno != EEXIST) {
        perror("mkdir");
        return -1;
    }

    return 0;
}

/**
 * fake_root_handle - Create a fake root handle file
 * 
 * This function creates a fake root handle file in the cache directory for the specified 
 * remote. It creates the necessary directory structure if it doesn't exist and then creates 
 * the root handle file. If the file already exists, it prints a message and does nothing.
 * 
 * Return: 0 on success, -1 on failure
 */
int
fake_root_handle() {
    char remote_root_handle_dir[4096];
    char remote_root_handle_file[8192];

    snprintf(remote_root_handle_dir, sizeof(remote_root_handle_dir), "%s/serve-nfs-handle-cache-disk/%s/01/00", CACHE_DIR, remote);
    snprintf(remote_root_handle_file, sizeof(remote_root_handle_file), "%s/%s", remote_root_handle_dir, ROOT_HANDLE_FILE);

    if (mkdirs(remote_root_handle_dir) != 0) return -1;

    int fd = open(remote_root_handle_file, O_CREAT | O_EXCL, 0644);
    if (fd < 0) {
        if (errno == EEXIST) {
            printf("root handle file %s already exists.\n", remote_root_handle_file);
        } else {
            perror("open");
            return -1;
        }
    } else {
        printf("root handle file %s created.\n", remote_root_handle_file);
        close(fd);
    }

    return 0;
}

/**
 * free_port - Find a free port on the system
 * 
 * This function creates a socket, binds it to port 0 to let the OS assign a free port, 
 * retrieves the assigned port number, and then closes the socket. It returns the free 
 * port number.
 * 
 * Return: The free port number on success, -1 on failure
 */
/*
int
free_port() {
    int found = 0;
    int sockfd = 0;
    struct sockaddr_in addr;
    socklen_t addr_len = sizeof(addr);

    sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) {
        perror("socket");
        return -1;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = 0; // Port 0 tells the OS to find a free port

    if (bind(sockfd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        close(sockfd);
        return -1;
    }

    if (getsockname(sockfd, (struct sockaddr *)&addr, &addr_len) < 0) {
        perror("getsockname");
        close(sockfd);
        return -1;
    }

    found = ntohs(addr.sin_port);

    close(sockfd);
    printf("found free port: %d.\n", found);

    return found;
}
*/

/**
 * is_port_free - Check if a port is free on the system
 * 
 * This function creates a socket, binds it to port and check EADDRINUSE 
 * 
 * Return: -1 on error, 1 if free 0 otherwise
 */
int is_port_free(int port) {
    int sockfd;
    struct sockaddr_in addr;

    sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) {
        perror("socket");
        return -1;
    }

    int opt = 1;
    setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = INADDR_ANY;

    int result = bind(sockfd, (struct sockaddr *)&addr, sizeof(addr));
    close(sockfd);

    if (result == 0) {
        return 1;  // Port is free
    } else if (errno == EADDRINUSE) {
        perror("bind");
        return 0;  // Port is in use
    } else {
        perror("bind");
        return -1; // Errror
    }
}


/**
 * mount_nfsd - Mounts the nfsd filesystem at /proc/fs/nfsd if not already mounted.
 *
 * This function checks if the nfsd virtual filesystem is already mounted at /proc/fs/nfsd
 * by reading /proc/mounts. If it is not mounted, it attempts to mount it using the mount()
 * system call.
 *
 * Return: 0 on success, -1 on failure
 */

int mount_nfsd() {
    /* check if already mounted */
    FILE *fp = fopen("/proc/mounts", "r");
    if (!fp) {
        perror("Failed to open /proc/mounts");
        return -1;
    }

    char line[512];
    while (fgets(line, sizeof(line), fp)) {
        char device[128], mount_point[128], fs_type[32];
        if (sscanf(line, "%127s %127s %31s", device, mount_point, fs_type) == 3) {
            if (strcmp(mount_point, PROC_NFSD) == 0 && strcmp(fs_type, "nfsd") == 0) {
                fclose(fp);
                printf("nfsd already mounted at %s\n", PROC_NFSD);
                return 0;
            }
        }
    }
    fclose(fp);

    /* not mounted, try to mount */
    if (mount("nfsd", PROC_NFSD, "nfsd", 0, NULL) == -1) {
        fprintf(stderr, "Failed to mount nfsd: %s\n", strerror(errno));
        return -1;
    }

    printf("Mounted nfsd on %s\n", PROC_NFSD);
    return 0;
}

/**
 * spawn_mountd - Spawn the mountd daemon process
 * 
 * This function spawns the mountd daemon process. It creates a new process, 
 * sets up a new session, and executes the rpc.mountd command. If the fork 
 * or exec fails, it prints an error message and returns -1.
 * 
 * Return: 0 on success, -1 on failure
 *
 * XXX: Too lazy for implementing double forking!!.
 */
int
spawn_mountd() {
    mountd_pid = fork();
    if (mountd_pid < 0) {
        perror("mountd fork");
        return -1;
    } else if (mountd_pid > 0) {
        printf("mountd started with PID: %d\n", mountd_pid);
        return rclone_pid;
    }

    /* Child process: Detach and execute rclone */
    if (setsid() < 0) {
        perror("setsid");
        _exit(127);
    }

    execlp("rpc.mountd", "rpc.mountd", "-F", (char *)NULL);

    /* if execlp fails */
    perror("mountd execlp");
    _exit(127);
}

/**
 * kill_mountd - Terminate the mountd daemon process
 * 
 * This function sends the SIGTERM signal to the mountd daemon process to terminate it. 
 * If the kill fails, it prints an error message and returns -1.
 * 
 * Return: 0 on success, -1 on failure
 */
int
kill_mountd() {
    if (kill(mountd_pid, SIGTERM) != 0) {
        perror("kill");
        return -1;
    }

    printf("mountd with PID %d has been terminated.\n", mountd_pid);
    return 0;
}

/**
 * spawn_rclone - Spawn an rclone server process
 * 
 * This function spawns an rclone server process with the specified remote and port number. 
 * It creates a new process, sets up a new session, and executes the rclone command. 
 * If the fork or exec fails, it prints an error message and returns -1.
 * 
 * Return: 0 on success, -1 on failure
 *
 * XXX: Too lazy for implementing double forking!!.
 */
int
spawn_rclone() {
    char address[24];
    char includes[8192] = "/{";

    snprintf(address, sizeof(address), "0.0.0.0:%d", port);

    for (struct Bucket *it = buckets; it; it = it->next) {
        /* + 6 to keep room for the "}/\** "suffix and null termination */
        if (strlen(includes) + strlen(it->name) + 6 > sizeof(includes)) {
            fprintf(stderr, "Bucket name(s) too long.\n");
            return -1;
        }
        strcat(includes, it->name);
        strcat(includes, ",");
    }

    /* We have some includes */
    if (buckets) {
        /* remove last ',' character */
        includes[strlen(includes) - 1] = '\0';
        strcat(includes, "}/** ");
    }

    rclone_pid = fork();
    if (rclone_pid < 0) {
        perror("rclone fork");
        return -1;
    } else if (rclone_pid > 0) {
        printf("rclone started with PID: %d\n", rclone_pid);
        return rclone_pid;
    }

    /* Child process: Detach and execute rclone */
    if (setsid() < 0) {
        perror("setsid");
        _exit(127);
    }

    if (buckets) {
        /* We have some includes */
        execlp("rclone", "rclone", "serve", "nfs", remote, "--include", includes, "--cache-dir", CACHE_DIR,
           "--addr", address, "--nfs-cache-type", "disk", "--vfs-cache-mode",
           "full", "--vfs-metadata-extension", ".metadata", "--links", (char *)NULL);
    } else {
        execlp("rclone", "rclone", "serve", "nfs", remote, "--cache-dir", CACHE_DIR,
           "--addr", address, "--nfs-cache-type", "disk", "--vfs-cache-mode",
           "full", "--vfs-metadata-extension", ".metadata", "--links", (char *)NULL);
    }

    /* if execlp fails */
    perror("rclone execlp");
    _exit(127);
}

/**
 * kill_rclone - Terminate the rclone server process
 * 
 * This function sends the SIGTERM signal to the rclone server process to terminate it. 
 * If the kill fails, it prints an error message and returns -1.
 * 
 * Return: 0 on success, -1 on failure
 */
int
kill_rclone() {
    if (kill(rclone_pid, SIGTERM) != 0) {
        perror("kill");
        return -1;
    }

    printf("successfully terminated rclone server with PID %d.\n", rclone_pid);
    return 0;
}

/**
 * exportfs - Export the rclone NFS filesystem
 * 
 * This function runs the exportfs command to export the rclone NFS filesystem 
 * with the specified options. If the command fails, it prints an error message 
 * and returns -1.
 * 
 * Return: 0 on success, -1 on failure
 */
int
exportfs() {
    if (anvil[0] != '\0') {
        char cmd[4096];
        sprintf(cmd, "exportfs -o rw,no_root_squash,sync,secure,mp %s:/export", anvil);
        if (system(cmd) !=0) {
            perror("failed to export to the anvil");
            return -1;
        }
    }

    if (system("exportfs -o rw,root_squash,sync,secure,mp *:/export") !=0) {
        perror("failed to export to the world");
        return -1;
    }

    printf("rclone NFS successfully exported.\n");
    return 0;
}

/**
 * unexportfs - Unexport the rclone NFS filesystem
 * 
 * This function runs the exportfs command to unexport the rclone NFS filesystem. 
 * If the command fails, it prints an error message and returns -1.
 * 
 * Return: 0 on success, -1 on failure
 */
int
unexportfs() {

    if (system("exportfs -u *:/export") != 0) {
        perror("failed to unexport to the world");
        return -1;
    }

    if (anvil[0] != '\0') {
        char cmd[4096];
        sprintf(cmd, "exportfs -u %s:/export", anvil);
        if (system(cmd) != 0) {
            perror("failed to unexport to the anvil");
            return -1;
        }
    }

    printf("rclone NFS successfully unexported.\n");
    return 0;
}

/**
 * register_rclone - Register the rclone NFS program with rpcbind
 * 
 * This function registers the rclone NFS program with rpcbind on the specified port. 
 * It first unregisters any existing instance of the program and then registers the new 
 * instance. If the registration fails, it prints an error message and returns -1.
 * 
 * Return: 0 on success, -1 on failure
 */
int
register_rclone() {
    pmap_unset(NFS_PROGRAM, NFS_VERSION);

    if (!pmap_set(NFS_PROGRAM, NFS_VERSION, IPPROTO_TCP, port)) {
        fprintf(stderr, "failed to register rclone NFS program with rpcbind.\n");
        return -1;
    }
    printf("rclone NFS program registered with rpcbind (prog: %ld, vers: %ld).\n", NFS_PROGRAM, NFS_VERSION);
    return 0;
}

/**
 * unregister_rclone - Unregister the rclone NFS program with rpcbind
 * 
 * This function unregisters the rclone NFS program from rpcbind.
 * It prints a message indicating the program has been unregistered.
 */
int
unregister_rclone() {
    if(!pmap_unset(NFS_PROGRAM, NFS_VERSION)){
        fprintf(stderr, "failed to unregister rclone NFS program from rpcbind.\n");
        return -1;
    }
    printf("unregistered rclone NFS program from rpcbind.\n");
    return 0;
}

/**
 * free_bucket - Free the bucket list
 * 
 * This function scan the list of bucket to free memory.
 */
void
free_buckets() {
    struct Bucket *b;
    while (buckets) {
        b = buckets;
        buckets = buckets->next;
        free(b);
    }
}

/**
 * startup - Start the rclone NFS service
 * 
 * This function initializes and starts the rclone NFS service. It finds a free port, 
 * creates a fake root handle, spawns the mountd and rclone processes, registers the 
 * rclone NFS program with rpcbind, and exports the NFS filesystem. If any step fails, 
 * it calls the teardown function and exits with an error code.
 */
void
startup() {
    /* TODO: We need a new driver allowing to set the port. 
     * Even then I don't know how we will advertise the port to the user??
     * Till then foce the port to 2049
     */ 
    /* port = free_port(); */
    port = 2049;
    if (port < 0) goto error;
    if (is_port_free(port) != 1) goto error;
    if (fake_root_handle() < 0) goto error;
    if (mount_nfsd() < 0) goto error;
    if (spawn_mountd() < 0) goto error;
    if (spawn_rclone() < 0) goto error;
    if (register_rclone() < 0) goto error;
    if (exportfs() < 0) goto error;
    printf("rclone NFS started.\n");
    return;
error:
    errno = 0;
    printf("Tearing down after error, ignore further errors.\n");
    teardown();
    exit(1);
}

/**
 * teardown - Stop the rclone NFS service
 * 
 * This function stops the rclone NFS service by unexporting the filesystem, 
 * unregistering the rclone NFS program, and terminating the rclone and mountd 
 * processes. It prints a message indicating the service has been stopped.
 */
void
teardown() {
    unexportfs();
    unregister_rclone();
    kill_rclone();
    kill_mountd();
    free_buckets();
    printf("rclone NFS program stopped.\n");
}

/**
 * trap - Signal handler for termination signals
 * @signum: The signal number
 * 
 * This function is a signal handler that catches termination signals (SIGINT and SIGTERM). 
 * It calls the teardown function to clean up and then exits the program.
 */
void
trap(int signum) {
    (void) signum; //XXX: check on signum
    teardown();
    exit(0);
}

/**
 * usage - Print help
 * 
 * This function display basic usage and exit.
 */
void usage() {
    fprintf(stderr, "usage: rcnfs [-a|--anvil <IP>] <REMOTE:[BUCKET:...]>\n");
    exit(2);
}

int
main(int argc, char **argv) {
    if (geteuid() != 0) {
        fprintf(stderr, "must be run as root.\n");
        exit(2);
    }

    struct option long_options[] = {
        {"anvil", required_argument, 0, 'a'},
        {0, 0, 0, 0}
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "a:", long_options, NULL)) != -1) {
        switch (opt) {
            case 'a':
                snprintf(anvil, sizeof(anvil), "%s", optarg);
                break;
            case '?':
                usage();
        }
    }

    if (argc == optind) {
        usage();
    }

    char* input = strdup(argv[optind]);
    if (!input) {
        perror("strdup failed");
        return 1;
    }

    char* token = strtok(input, ":");
    if (!token) {
        fprintf(stderr, "Error: invalid input format\n");
        free(input);
        return 1;
    }

    snprintf(remote, sizeof(remote), "%s:", token);
    remote[sizeof(remote) - 1] = '\0';

    while ((token = strtok(NULL, ":")) != NULL) {
        struct Bucket* bucket = malloc(sizeof(struct Bucket));
        if (!bucket) {
            perror("malloc failed");
            free_buckets();
            free(input);
            return 1;
        }
        strncpy(bucket->name, token, sizeof(bucket->name));
        bucket->name[sizeof(bucket->name) - 1] = '\0';
        bucket->next = buckets;
        buckets = bucket;
    }

    startup();

    signal(SIGINT, trap);
    signal(SIGTERM, trap);

    printf("entering main loop...\n");
    while (1) { sleep(60); }
    printf("bye...\n");

    return 0;
}
