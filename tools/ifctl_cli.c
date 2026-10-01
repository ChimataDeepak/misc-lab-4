/* ifctl_cli <ifname> show|up|down|mtu <n>  -- tiny ifconfig built on lib/ifctl.
 * Handy to watch with: strace -e trace=ioctl ./ifctl_cli wwan0 up */
#include "ifctl.h"

#include <net/if.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: %s <ifname> show|up|down|mtu <n>\n", argv[0]);
        return 2;
    }
    const char *ifn = argv[1], *cmd = argv[2];
    int rc;
    if (!strcmp(cmd, "up") || !strcmp(cmd, "down"))
        rc = ifctl_set_up(ifn, !strcmp(cmd, "up"));
    else if (!strcmp(cmd, "mtu") && argc > 3)
        rc = ifctl_set_mtu(ifn, atoi(argv[3]));
    else if (!strcmp(cmd, "show")) {
        unsigned f = 0;
        int mtu = 0;
        rc = ifctl_get_flags(ifn, &f);
        if (!rc)
            rc = ifctl_get_mtu(ifn, &mtu);
        if (!rc)
            printf("%s flags=0x%x %s mtu=%d\n", ifn, f, (f & IFF_UP) ? "UP" : "DOWN", mtu);
    } else {
        fprintf(stderr, "bad command\n");
        return 2;
    }
    if (rc) {
        fprintf(stderr, "%s: %s\n", ifn, strerror(-rc));
        return 1;
    }
    return 0;
}
