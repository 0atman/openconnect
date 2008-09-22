/*
 * Open AnyConnect (SSL + DTLS) client
 *
 * © 2008 David Woodhouse <dwmw2@infradead.org>
 *
 * Permission to use, copy, modify, and/or distribute this software
 * for any purpose with or without fee is hereby granted, provided
 * that the above copyright notice and this permission notice appear
 * in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL
 * WARRANTIES WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE
 * AUTHOR BE LIABLE FOR ANY SPECIAL, DIRECT, INDIRECT, OR
 * CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS
 * OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT,
 * NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN
 * CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <string.h>
#include <net/if.h>
#include <sys/ioctl.h>
#include <linux/if_tun.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

#include "anyconnect.h"

/* Set up a tuntap device. */
int setup_tun(struct anyconnect_info *vpninfo)
{
	struct vpn_option *cstp_opt = vpninfo->cstp_options;
	struct ifreq ifr;
	int tun_fd;

	tun_fd = open("/dev/net/tun", O_RDWR);
	if (tun_fd == -1) {
		perror("open tun");
		exit(1);
	}
	memset(&ifr, 0, sizeof(ifr));
	ifr.ifr_flags = IFF_TUN | IFF_NO_PI;
	strncpy(ifr.ifr_name, "cisco0", sizeof(ifr.ifr_name) - 1);
	if (ioctl(tun_fd, TUNSETIFF, (void *) &ifr) < 0){
		perror("TUNSETIFF");
		exit(1);
	}

	/* FIXME: Configure it... */
	while (cstp_opt) {
		printf("CSTP option %s : %s\n", cstp_opt->option, cstp_opt->value);
		cstp_opt = cstp_opt->next;
	}

	/* Better still, use lwip and just provide a SOCKS server rather than
	   telling the kernel about it at all */
	vpninfo->tun_fd = tun_fd;
	return 0;
}

