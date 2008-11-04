/*
 * Open AnyConnect (SSL + DTLS) client
 *
 * © 2008 David Woodhouse <dwmw2@infradead.org>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public License
 * as published by the Free Software Foundation; either version 2.1 of
 * the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to:
 *
 *   Free Software Foundation, Inc.
 *   51 Franklin Street, Fifth Floor,
 *   Boston, MA 02110-1301 USA
 */

#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <pwd.h>
#include <sys/syslog.h>
#include <sys/utsname.h>
#include <sys/types.h>
#include <openssl/rand.h>

#define _GNU_SOURCE
#include <getopt.h>

#include "openconnect.h"

static int write_new_config(struct openconnect_info *vpninfo, char *buf, int buflen);
static void write_progress(struct openconnect_info *info, int level, const char *fmt, ...);
static void syslog_progress(struct openconnect_info *info, int level, const char *fmt, ...);

int verbose = PRG_INFO;

static struct option long_options[] = {
	{"certificate", 1, 0, 'c'},
	{"sslkey", 1, 0, 'k'},
	{"cookie", 1, 0, 'C'},
	{"deflate", 0, 0, 'd'},
	{"no-deflate", 0, 0, 'D'},
	{"help", 0, 0, 'h'},
	{"interface", 1, 0, 'i'},
	{"mtu", 1, 0, 'm'},
	{"setuid", 1, 0, 'U'},
	{"script", 1, 0, 's'},
	{"script-tun", 0, 0, 'S'},
	{"syslog", 0, 0, 'l'},
	{"tpm-key", 0, 0, 't'},
	{"tpm-password", 1, 0, 'p'},
	{"user", 1, 0, 'u'},
	{"verbose", 0, 0, 'v'},
	{"version", 0, 0, 'V'},
	{"cafile", 1, 0, '0'},
	{"no-dtls", 0, 0, '1'},
	{"cookieonly", 0, 0, '2'},
	{"printcookie", 0, 0, '3'},
	{"quiet", 0, 0, 'q'},
	{"queue-len", 1, 0, 'Q'},
	{"xmlconfig", 1, 0, 'x'},
	{"cookie-on-stdin", 0, 0, '4'},
	{"passwd-on-stdin", 0, 0, '5'},
	{"no-passwd", 0, 0, '6'},
	{NULL, 0, 0, 0},
};

void usage(void)
{
	printf("Usage:  openconnect [options] <server>\n");
	printf("Open client for Cisco AnyConnect VPN, version %s\n\n", openconnect_version);
	printf("  -c, --certificate=CERT          Use SSL client certificate CERT\n");
	printf("  -k, --sslkey=KEY                Use SSL private key file KEY\n");
	printf("  -C, --cookie=COOKIE             Use WebVPN cookie COOKIE\n");
	printf("      --cookie-on-stdin           Read cookie from standard input\n");
	printf("  -d, --deflate                   Enable compression (default)\n");
	printf("  -D, --no-deflate                Disable compression\n");
	printf("  -h, --help                      Display help text\n");
	printf("  -i, --interface=IFNAME          Use IFNAME for tunnel interface\n");
	printf("  -l, --syslog                    Use syslog for progress messages\n");
	printf("  -U, --setuid=USER               Drop privileges after connecting\n");
	printf("  -m, --mtu=MTU                   Request MTU from server\n");
	printf("  -p, --tpm-password=PASS         Set TPM SRK PIN\n");
	printf("  -q, --quiet                     Less output\n");
	printf("  -Q, --queue-len=LEN             Set packet queue limit to LEN pkts\n");
	printf("  -s, --script=SCRIPT             Use vpnc-compatible config script\n");
	printf("  -S, --script-tun                Pass traffic to 'script' program, not tun\n");
	printf("  -t, --tpm                       Use TPM engine for private key\n");
	printf("  -u, --user=NAME                 Set login username\n");
	printf("  -V, --version                   Report version number\n");
	printf("  -v, --verbose                   More output\n");
	printf("  -x, --xmlconfig=CONFIG          XML config file\n");
	printf("      --cookieonly                Fetch webvpn cookie only; don't connect\n");
	printf("      --printcookie               Print webvpn cookie before connecting\n");
	printf("      --cafile=FILE               Cert file for server verification\n");
	printf("      --no-dtls                   Disable DTLS\n");
	printf("      --no-passwd                 Disable password/SecurID authentication\n");
	printf("      --passwd-on-stdin           Read password from standard input\n");
	exit(1);
}

static void read_stdin(char **string)
{
	char *c = malloc(100);
	if (!c) {
		fprintf(stderr, "Allocation failure for string from stdin\n");
		exit(1);
	}
	if (!fgets(c, 100, stdin)) {
		perror("fgets (stdin)");
		exit(1);
	}

	*string = c;

	c = strchr(*string, '\n');
	if (c)
		*c = 0;
}

int main(int argc, char **argv)
{
	struct openconnect_info *vpninfo;
	struct utsname utsbuf;
	int cookieonly = 0;
	int use_syslog = 0;
	uid_t uid = getuid();
	int opt;

	openconnect_init_openssl();

	vpninfo = malloc(sizeof(*vpninfo));
	if (!vpninfo) {
		fprintf(stderr, "Failed to allocate vpninfo structure\n");
		exit(1);
	}
	memset(vpninfo, 0, sizeof(*vpninfo));

	/* Set up some defaults */
	vpninfo->tun_fd = vpninfo->ssl_fd = vpninfo->dtls_fd = vpninfo->new_dtls_fd = -1;
	vpninfo->useragent = openconnect_create_useragent("Open AnyConnect VPN Agent %s");
	vpninfo->mtu = 1406;
	vpninfo->deflate = 1;
	vpninfo->dtls_attempt_period = 60;
	vpninfo->max_qlen = 10;

	if (RAND_bytes(vpninfo->dtls_secret, sizeof(vpninfo->dtls_secret)) != 1) {
		fprintf(stderr, "Failed to initialise DTLS secret\n");
		exit(1);
	}
	if (!uname(&utsbuf))
		vpninfo->localname = utsbuf.nodename;
	else
		vpninfo->localname = "localhost";

	while ((opt = getopt_long(argc, argv, "C:c:Ddhi:k:lp:Q:qSs:tU:u:Vvx:",
				  long_options, NULL))) {
		if (opt < 0)
			break;

		switch (opt) {
		case '0':
			vpninfo->cafile = optarg;
			break;
		case '1':
			vpninfo->dtls_attempt_period = 0;
			break;
		case '2':
			cookieonly = 1;
			break;
		case '3':
			cookieonly = 2;
			break;
		case '4':
			read_stdin(&vpninfo->cookie);
			/* If the cookie is empty, ignore it */
			if (! *vpninfo->cookie) {
				vpninfo->cookie = NULL;
			}
			break;
		case '5':
			read_stdin(&vpninfo->password);
			break;
		case '6':
			vpninfo->nopasswd = 1;
			break;
		case 'C':
			vpninfo->cookie = optarg;
			break;
		case 'c':
			vpninfo->cert = optarg;
			break;
		case 'k':
			vpninfo->sslkey = optarg;
			break;
		case 'd':
			vpninfo->deflate = 1;
			break;
		case 'D':
			vpninfo->deflate = 0;
			break;
		case 'h':
			usage();
		case 'i':
			vpninfo->ifname = optarg;
			break;
		case 'l':
			use_syslog = 1;
			break;
		case 'm':
			vpninfo->mtu = atol(optarg);
			if (vpninfo->mtu < 576) {
				fprintf(stderr, "MTU %d too small\n", vpninfo->mtu);
				vpninfo->mtu = 576;
			}
			break;
		case 'p':
			vpninfo->tpmpass = optarg;
			break;
		case 's':
			vpninfo->vpnc_script = optarg;
			break;
		case 'S':
			vpninfo->script_tun = 1;
			break;
		case 't':
			vpninfo->tpm = 1;
			break;
		case 'u':
			vpninfo->username = optarg;
			break;
		case 'U': {
			char *strend;
			uid = strtol(optarg, &strend, 0);
			if (strend[0]) {
				struct passwd *pw = getpwnam(optarg);
				if (!pw) {
					fprintf(stderr, "Invalid user \"%s\"\n",
						optarg);
					exit(1);
				}
				uid = pw->pw_uid;
			}
			break;
		}
		case 'Q':
			vpninfo->max_qlen = atol(optarg);
			if (!vpninfo->max_qlen) {
				fprintf(stderr, "Queue length zero not permitted; using 1\n");
				vpninfo->max_qlen = 1;
			}
			break;
		case 'q':
			verbose = PRG_ERR;
			break;
		case 'v':
			verbose = PRG_TRACE;
			break;
		case 'V':
			printf("OpenConnect version %s\n", openconnect_version);
			exit(0);
		case 'x':
			vpninfo->xmlconfig = optarg;
			vpninfo->write_new_config = write_new_config;
			break;
		default:
			usage();
		}
	}
	if (optind != argc - 1) {
		fprintf(stderr, "No server specified\n");
		usage();
	}

	if (!vpninfo->sslkey)
		vpninfo->sslkey = vpninfo->cert;

	if (use_syslog) {
		openlog("openconnect", LOG_PID, LOG_DAEMON);
		vpninfo->progress = syslog_progress;
	} else {
		vpninfo->progress = write_progress;
	}

	if (config_lookup_host(vpninfo, argv[optind]))
		exit(1);

	if (!vpninfo->hostname)
		vpninfo->hostname = strdup(argv[optind]);
	vpninfo->urlpath = strdup("/");

#ifdef SSL_UI
	set_openssl_ui();
#endif

	if (!vpninfo->cookie && openconnect_obtain_cookie(vpninfo)) {
		fprintf(stderr, "Failed to obtain WebVPN cookie\n");
		exit(1);
	}

	if (cookieonly) {
		printf("%s\n", vpninfo->cookie);
		if (cookieonly == 1)
			/* We use cookieonly=2 for 'print it and continue' */
			exit(0);
	}
	if (make_cstp_connection(vpninfo)) {
		fprintf(stderr, "Creating SSL connection failed\n");
		exit(1);
	}
	
	if (setup_tun(vpninfo)) {
		fprintf(stderr, "Set up tun device failed\n");
		exit(1);
	}

	if (uid != getuid()) {
		if (setuid(uid)) {
			fprintf(stderr, "Failed to set uid %d\n", uid);
			exit(1);
		}
	}
		
	if (vpninfo->dtls_attempt_period && setup_dtls(vpninfo))
		fprintf(stderr, "Set up DTLS failed; using SSL instead\n");

	vpninfo->progress(vpninfo, PRG_INFO,
			  "Connected %s as %s, using %s\n", vpninfo->ifname,
			  vpninfo->vpn_addr,
			  (vpninfo->dtls_fd==-1)?(vpninfo->deflate?"SSL + deflate":"SSL"):"DTLS");

	vpn_mainloop(vpninfo);
	exit(1);
}

static int write_new_config(struct openconnect_info *vpninfo, char *buf, int buflen)
{
	int config_fd;

	config_fd = open(vpninfo->xmlconfig, O_WRONLY|O_TRUNC|O_CREAT, 0644);
	if (!config_fd) {
		fprintf(stderr, "Failed to open %s for write: %s\n", 
			vpninfo->xmlconfig, strerror(errno));
		return -errno;
	}

	/* FIXME: We should actually write to a new tempfile, then rename */
	write(config_fd, buf, buflen);	
	return 0;
}

void write_progress(struct openconnect_info *info, int level, const char *fmt, ...)
{
	FILE *outf = level?stdout:stderr;
	va_list args;

	if (verbose >= level) {
		va_start(args, fmt);
		vfprintf(outf, fmt, args);
		va_end(args);
	}
}

void syslog_progress(struct openconnect_info *info, int level,
		     const char *fmt, ...)
{
	int priority = level?LOG_INFO:LOG_NOTICE;
	va_list args;

	if (verbose >= level) {
		va_start(args, fmt);
		vsyslog(priority, fmt, args);
		va_end(args);
	}
}
