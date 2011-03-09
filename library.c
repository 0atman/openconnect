/*
 * OpenConnect (SSL + DTLS) VPN client
 *
 * Copyright © 2008-2011 Intel Corporation.
 *
 * Authors: David Woodhouse <dwmw2@infradead.org>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public License
 * version 2.1, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
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

#include "openconnect-internal.h"

struct openconnect_info *openconnect_vpninfo_new (char *useragent,
						  openconnect_validate_peer_cert_fn validate_peer_cert,
						  openconnect_write_new_config_fn write_new_config,
						  openconnect_process_auth_form_fn process_auth_form,
						  openconnect_progress_fn progress)
{
	struct openconnect_info *vpninfo = calloc (sizeof(*vpninfo), 1);

	vpninfo->mtu = 1406;
	vpninfo->ssl_fd = -1;
	vpninfo->useragent = openconnect_create_useragent (useragent);
	vpninfo->validate_peer_cert = validate_peer_cert;
	vpninfo->write_new_config = write_new_config;
	vpninfo->process_auth_form = process_auth_form;
	vpninfo->progress = progress;

	return vpninfo;
}

char *openconnect_get_hostname (struct openconnect_info *vpninfo)
{
	return vpninfo->hostname;
}

void openconnect_set_hostname (struct openconnect_info *vpninfo, char *hostname)
{
	vpninfo->hostname = hostname;
}

char *openconnect_get_urlpath (struct openconnect_info *vpninfo)
{
	return vpninfo->urlpath;
}

void openconnect_set_urlpath (struct openconnect_info *vpninfo, char *urlpath)
{
	vpninfo->urlpath = urlpath;
}

void openconnect_set_xmlsha1 (struct openconnect_info *vpninfo, char *xmlsha1, int size)
{
	if (size != sizeof (vpninfo->xmlsha1))
		return;

	memcpy (&vpninfo->xmlsha1, xmlsha1, size);

}

void openconnect_set_cafile (struct openconnect_info *vpninfo, char *cafile)
{
	vpninfo->cafile = cafile;
}

void openconnect_setup_csd (struct openconnect_info *vpninfo, uid_t uid, int silent, char *wrapper)
{
	vpninfo->uid_csd = uid;
	vpninfo->uid_csd_given = silent?2:1;
	vpninfo->csd_wrapper = wrapper;
}

void openconnect_set_client_cert (struct openconnect_info *vpninfo, char *cert, char *sslkey)
{
	vpninfo->cert = cert;
	if (sslkey)
		vpninfo->sslkey = sslkey;
	else
		vpninfo->sslkey = cert;
}

struct x509_st *openconnect_get_peer_cert (struct openconnect_info *vpninfo)
{
	return SSL_get_peer_certificate(vpninfo->https_ssl);
}

int openconnect_get_port (struct openconnect_info *vpninfo)
{
	return vpninfo->port;
}

char *openconnect_get_cookie (struct openconnect_info *vpninfo)
{
	return vpninfo->cookie;
}

void openconnect_clear_cookie (struct openconnect_info *vpninfo)
{
	memset(vpninfo->cookie, 0, sizeof(vpninfo->cookie));
}

void openconnect_reset_ssl (struct openconnect_info *vpninfo)
{
	if (vpninfo->https_ssl) {
		free(vpninfo->peer_addr);
		vpninfo->peer_addr = NULL;
		openconnect_close_https(vpninfo);
	}
	if (vpninfo->https_ctx) {
		SSL_CTX_free(vpninfo->https_ctx);
		vpninfo->https_ctx = NULL;
	}
}

int openconnect_parse_url (struct openconnect_info *vpninfo, char *url)
{
	return internal_parse_url (url, NULL, &vpninfo->hostname,
				   &vpninfo->port, &vpninfo->urlpath, 443);
}
