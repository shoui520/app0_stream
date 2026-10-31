#ifndef A0S_VFS_H
#define A0S_VFS_H

#define A0S_VFS_NAME	"app0stream_netfs"
#define A0S_ASSIGN	"net0:"

/* net0:/.m<n> is the root again, under a name of its own: each PFS mount
 * gets a lower directory nothing else covers */
#define A0S_ALIAS	".m"

int a0sVfsRegister(void);
int a0sVfsMount(void);

#endif
