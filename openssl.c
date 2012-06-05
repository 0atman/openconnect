/*
 * OpenConnect (SSL + DTLS) VPN client
 *
 * Copyright © 2008-2012 Intel Corporation.
 *
 * Author: David Woodhouse <dwmw2@infradead.org>
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

#include <errno.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <ctype.h>

#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/engine.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <openssl/pkcs12.h>
#include <openssl/x509v3.h>
#include <openssl/x509.h>
#include <openssl/bio.h>

#include "openconnect-internal.h"

int openconnect_sha1(unsigned char *result, void *data, int len)
{
        EVP_MD_CTX c;

        EVP_MD_CTX_init(&c);
        EVP_Digest(data, len, result, NULL, EVP_sha1(), NULL);
        EVP_MD_CTX_cleanup(&c);

        return 0;
}

int openconnect_get_cert_DER(struct openconnect_info *vpninfo,
			     struct x509_st *cert, unsigned char **buf)
{
	BIO *bp = BIO_new(BIO_s_mem());
	BUF_MEM *certinfo;
	size_t l;

	if (!i2d_X509_bio(bp, cert)) {
		BIO_free(bp);
		return -EIO;
	}

	BIO_get_mem_ptr(bp, &certinfo);
	l = certinfo->length;
	*buf = malloc(l);
	if (!*buf) {
		BIO_free(bp);
		return -ENOMEM;
	}
	memcpy(*buf, certinfo->data, l);
	BIO_free(bp);
	return l;
}

int openconnect_random(void *bytes, int len)
{
	if (RAND_bytes(bytes, len) != 1)
		return -EIO;
	return 0;
}

/* Helper functions for reading/writing lines over SSL.
   We could use cURL for the HTTP stuff, but it's overkill */

int openconnect_SSL_write(struct openconnect_info *vpninfo, char *buf, size_t len)
{
	size_t orig_len = len;

	while (len) {
		int done = SSL_write(vpninfo->https_ssl, buf, len);

		if (done > 0)
			len -= done;
		else {
			int err = SSL_get_error(vpninfo->https_ssl, done);
			fd_set wr_set, rd_set;
			int maxfd = vpninfo->ssl_fd;

			FD_ZERO(&wr_set);
			FD_ZERO(&rd_set);
			
			if (err == SSL_ERROR_WANT_READ)
				FD_SET(vpninfo->ssl_fd, &rd_set);
			else if (err == SSL_ERROR_WANT_WRITE)
				FD_SET(vpninfo->ssl_fd, &wr_set);
			else {
				vpn_progress(vpninfo, PRG_ERR, _("Failed to write to SSL socket"));
				openconnect_report_ssl_errors(vpninfo);
				return -EIO;
			}
			if (vpninfo->cancel_fd != -1) {
				FD_SET(vpninfo->cancel_fd, &rd_set);
				if (vpninfo->cancel_fd > vpninfo->ssl_fd)
					maxfd = vpninfo->cancel_fd;
			}
			select(maxfd + 1, &rd_set, &wr_set, NULL, NULL);
			if (vpninfo->cancel_fd != -1 &&
			    FD_ISSET(vpninfo->cancel_fd, &rd_set)) {
				vpn_progress(vpninfo, PRG_ERR, _("SSL write cancelled\n"));
				return -EINTR;
			}
		}
	}
	return orig_len;
}

int openconnect_SSL_read(struct openconnect_info *vpninfo, char *buf, size_t len)
{
	int done;

	while ((done = SSL_read(vpninfo->https_ssl, buf, len)) == -1) {
		int err = SSL_get_error(vpninfo->https_ssl, done);
		fd_set wr_set, rd_set;
		int maxfd = vpninfo->ssl_fd;

		FD_ZERO(&wr_set);
		FD_ZERO(&rd_set);
			
		if (err == SSL_ERROR_WANT_READ)
			FD_SET(vpninfo->ssl_fd, &rd_set);
		else if (err == SSL_ERROR_WANT_WRITE)
			FD_SET(vpninfo->ssl_fd, &wr_set);
		else {
			vpn_progress(vpninfo, PRG_ERR, _("Failed to read from SSL socket"));
			openconnect_report_ssl_errors(vpninfo);
			return -EIO;
		}
		if (vpninfo->cancel_fd != -1) {
			FD_SET(vpninfo->cancel_fd, &rd_set);
			if (vpninfo->cancel_fd > vpninfo->ssl_fd)
				maxfd = vpninfo->cancel_fd;
		}
		select(maxfd + 1, &rd_set, &wr_set, NULL, NULL);
		if (vpninfo->cancel_fd != -1 &&
		    FD_ISSET(vpninfo->cancel_fd, &rd_set)) {
			vpn_progress(vpninfo, PRG_ERR, _("SSL read cancelled\n"));
			return -EINTR;
		}
	}
	return done;
}

static int print_err(const char *str, size_t len, void *ptr)
{
	struct openconnect_info *vpninfo = ptr;

	vpn_progress(vpninfo, PRG_ERR, "%s", str);
	return 0;
}

void openconnect_report_ssl_errors(struct openconnect_info *vpninfo)
{
	ERR_print_errors_cb(print_err, vpninfo);
}

int openconnect_SSL_gets(struct openconnect_info *vpninfo, char *buf, size_t len)
{
	int i = 0;
	int ret;

	if (len < 2)
		return -EINVAL;

	while (1) {
		ret = SSL_read(vpninfo->https_ssl, buf + i, 1);
		if (ret == 1) {
			if (buf[i] == '\n') {
				buf[i] = 0;
				if (i && buf[i-1] == '\r') {
					buf[i-1] = 0;
					i--;
				}
				return i;
			}
			i++;

			if (i >= len - 1) {
				buf[i] = 0;
				return i;
			}
		} else {
			fd_set rd_set, wr_set;
			int maxfd = vpninfo->ssl_fd;
			
			FD_ZERO(&rd_set);
			FD_ZERO(&wr_set);
			
			ret = SSL_get_error(vpninfo->https_ssl, ret);
			if (ret == SSL_ERROR_WANT_READ)
				FD_SET(vpninfo->ssl_fd, &rd_set);
			else if (ret == SSL_ERROR_WANT_WRITE)
				FD_SET(vpninfo->ssl_fd, &wr_set);
			else {
				vpn_progress(vpninfo, PRG_ERR, _("Failed to read from SSL socket\n"));
				openconnect_report_ssl_errors(vpninfo);
				ret = -EIO;
				break;
			}
			if (vpninfo->cancel_fd != -1) {
				FD_SET(vpninfo->cancel_fd, &rd_set);
				if (vpninfo->cancel_fd > vpninfo->ssl_fd)
					maxfd = vpninfo->cancel_fd;
			}
			select(maxfd + 1, &rd_set, &wr_set, NULL, NULL);
			if (vpninfo->cancel_fd != -1 &&
			    FD_ISSET(vpninfo->cancel_fd, &rd_set)) {
				vpn_progress(vpninfo, PRG_ERR, _("SSL read cancelled\n"));
				ret = -EINTR;
				break;
			}
		}
	}
	buf[i] = 0;
	return i ?: ret;
}

static int pem_pw_cb(char *buf, int len, int w, void *v)
{
	struct openconnect_info *vpninfo = v;
	char *pass = NULL;
	int plen;

	if (vpninfo->cert_password) {
		pass = vpninfo->cert_password;
		vpninfo->cert_password = NULL;
	} else if (request_passphrase(vpninfo, &pass,
				      _("Enter PEM pass phrase:")))
		return -1;

	plen = strlen(pass);

	if (len <= plen) {
		vpn_progress(vpninfo, PRG_ERR,
			     _("PEM password too long (%d >= %d)\n"),
			     plen, len);
		free(pass);
		return -1;
	}

	memcpy(buf, pass, plen+1);
	free(pass);
	return plen;
}

static int load_pkcs12_certificate(struct openconnect_info *vpninfo, PKCS12 *p12)
{
	EVP_PKEY *pkey = NULL;
	X509 *cert = NULL;
	STACK_OF(X509) *ca;
	int ret = 0;
	char *pass;

	pass = vpninfo->cert_password;
	vpninfo->cert_password = NULL;
 retrypass:
	/* We do this every time round the loop, to work around a bug in
	   OpenSSL < 1.0.0-beta2 -- where the stack at *ca will be freed
	   when PKCS12_parse() returns an error, but *ca is left pointing
	   to the freed memory. */
	ca = NULL;
	if (!pass && request_passphrase(vpninfo, &pass,
					_("Enter PKCS#12 pass phrase:")) < 0) {
		PKCS12_free(p12);
		return -EINVAL;
	}
	if (!PKCS12_parse(p12, pass, &pkey, &cert, &ca)) {
		unsigned long err = ERR_peek_error();

		openconnect_report_ssl_errors(vpninfo);

		if (ERR_GET_LIB(err) == ERR_LIB_PKCS12 &&
		    ERR_GET_FUNC(err) == PKCS12_F_PKCS12_PARSE &&
		    ERR_GET_REASON(err) == PKCS12_R_MAC_VERIFY_FAILURE) {
			vpn_progress(vpninfo, PRG_ERR,
				     _("Parse PKCS#12 failed (wrong passphrase?)\n"));
			free(pass);
			pass = NULL;
			goto retrypass;
		}

		vpn_progress(vpninfo, PRG_ERR,
			     _("Parse PKCS#12 failed (see above errors)\n"));
		PKCS12_free(p12);
		return -EINVAL;
	}
	if (cert) {
		vpninfo->cert_x509 = cert;
		SSL_CTX_use_certificate(vpninfo->https_ctx, cert);
	} else {
		vpn_progress(vpninfo, PRG_ERR,
			     _("PKCS#12 contained no certificate!"));
		ret = -EINVAL;
	}

	if (pkey) {
		SSL_CTX_use_PrivateKey(vpninfo->https_ctx, pkey);
		EVP_PKEY_free(pkey);
	} else {
		vpn_progress(vpninfo, PRG_ERR,
			     _("PKCS#12 contained no private key!"));
		ret = -EINVAL;
	}

	/* Only include supporting certificates which are actually necessary */
	if (ca) {
		int i;
	next:
		for (i = 0; i < sk_X509_num(ca); i++) {
			X509 *cert2 = sk_X509_value(ca, i);
			if (X509_check_issued(cert2, cert) == X509_V_OK) {
				char buf[200];

				if (cert2 == cert)
					break;
				if (X509_check_issued(cert2, cert2) == X509_V_OK)
					break;

				X509_NAME_oneline(X509_get_subject_name(cert2),
						  buf, sizeof(buf));
				vpn_progress(vpninfo, PRG_DEBUG,
					     _("Extra cert from PKCS#12: '%s'\n"), buf);
				CRYPTO_add(&cert2->references, 1, CRYPTO_LOCK_X509);
				SSL_CTX_add_extra_chain_cert(vpninfo->https_ctx, cert2);
				cert = cert2;
				goto next;
			}
		}
		sk_X509_pop_free(ca, X509_free);
	}

	PKCS12_free(p12);
	return ret;
}

#ifdef HAVE_ENGINE
static int load_tpm_certificate(struct openconnect_info *vpninfo)
{
	ENGINE *e;
	EVP_PKEY *key;
	ENGINE_load_builtin_engines();

	e = ENGINE_by_id("tpm");
	if (!e) {
		vpn_progress(vpninfo, PRG_ERR, _("Can't load TPM engine.\n"));
		openconnect_report_ssl_errors(vpninfo);
		return -EINVAL;
	}
	if (!ENGINE_init(e) || !ENGINE_set_default_RSA(e) ||
	    !ENGINE_set_default_RAND(e)) {
		vpn_progress(vpninfo, PRG_ERR, _("Failed to init TPM engine\n"));
		openconnect_report_ssl_errors(vpninfo);
		ENGINE_free(e);
		return -EINVAL;
	}

	if (vpninfo->cert_password) {
		if (!ENGINE_ctrl_cmd(e, "PIN", strlen(vpninfo->cert_password),
				     vpninfo->cert_password, NULL, 0)) {
			vpn_progress(vpninfo, PRG_ERR,
				     _("Failed to set TPM SRK password\n"));
			openconnect_report_ssl_errors(vpninfo);
		}
	}
	key = ENGINE_load_private_key(e, vpninfo->sslkey, NULL, NULL);
	if (!key) {
		vpn_progress(vpninfo, PRG_ERR,
			     _("Failed to load TPM private key\n"));
		openconnect_report_ssl_errors(vpninfo);
		ENGINE_free(e);
		ENGINE_finish(e);
		return -EINVAL;
	}
	if (!SSL_CTX_use_PrivateKey(vpninfo->https_ctx, key)) {
		vpn_progress(vpninfo, PRG_ERR, _("Add key from TPM failed\n"));
		openconnect_report_ssl_errors(vpninfo);
		ENGINE_free(e);
		ENGINE_finish(e);
		return -EINVAL;
	}
	return 0;
}
#else
static int load_tpm_certificate(struct openconnect_info *vpninfo)
{
	vpn_progress(vpninfo, PRG_ERR,
		     _("This version of OpenConnect was built without TPM support\n"));
	return -EINVAL;
}
#endif

static int reload_pem_cert(struct openconnect_info *vpninfo)
{
	BIO *b = BIO_new(BIO_s_file_internal());

	if (!b)
		return -ENOMEM;

	if (BIO_read_filename(b, vpninfo->cert) <= 0) {
	err:
		BIO_free(b);
		vpn_progress(vpninfo, PRG_ERR,
			     _("Failed to reload X509 cert for expiry check\n"));
		openconnect_report_ssl_errors(vpninfo);
		return -EIO;
	}
	vpninfo->cert_x509 = PEM_read_bio_X509_AUX(b, NULL, NULL, NULL);
	if (!vpninfo->cert_x509)
		goto err;

	return 0;
}

static int load_certificate(struct openconnect_info *vpninfo)
{
	vpn_progress(vpninfo, PRG_TRACE,
		     _("Using certificate file %s\n"), vpninfo->cert);

	if (vpninfo->cert_type == CERT_TYPE_PKCS12 ||
	    vpninfo->cert_type == CERT_TYPE_UNKNOWN) {
		FILE *f;
		PKCS12 *p12;

		f = fopen(vpninfo->cert, "r");
		if (!f) {
			vpn_progress(vpninfo, PRG_ERR,
				     _("Failed to open certificate file %s: %s\n"),
				     vpninfo->cert, strerror(errno));
			return -ENOENT;
		}
		p12 = d2i_PKCS12_fp(f, NULL);
		fclose(f);
		if (p12)
			return load_pkcs12_certificate(vpninfo, p12);

		/* Not PKCS#12 */
		if (vpninfo->cert_type == CERT_TYPE_PKCS12) {
			vpn_progress(vpninfo, PRG_ERR, _("Read PKCS#12 failed\n"));
			openconnect_report_ssl_errors(vpninfo);
			return -EINVAL;
		}
		/* Clear error and fall through to see if it's a PEM file... */
		ERR_clear_error();
	}

	/* It's PEM or TPM now, and either way we need to load the plain cert: */
	if (!SSL_CTX_use_certificate_chain_file(vpninfo->https_ctx,
						vpninfo->cert)) {
		vpn_progress(vpninfo, PRG_ERR,
			     _("Loading certificate failed\n"));
		openconnect_report_ssl_errors(vpninfo);
		return -EINVAL;
	}

	/* Ew, we can't get it back from the OpenSSL CTX in any sane fashion */
	reload_pem_cert(vpninfo);

	if (vpninfo->cert_type == CERT_TYPE_UNKNOWN) {
		FILE *f = fopen(vpninfo->sslkey, "r");
		char buf[256];

		if (!f) {
			vpn_progress(vpninfo, PRG_ERR,
				     _("Failed to open private key file %s: %s\n"),
				     vpninfo->cert, strerror(errno));
			return -ENOENT;
		}

		buf[255] = 0;
		while (fgets(buf, 255, f)) {
			if (!strcmp(buf, "-----BEGIN TSS KEY BLOB-----\n")) {
				vpninfo->cert_type = CERT_TYPE_TPM;
				break;
			} else if (!strcmp(buf, "-----BEGIN RSA PRIVATE KEY-----\n") ||
				   !strcmp(buf, "-----BEGIN DSA PRIVATE KEY-----\n") ||
				   !strcmp(buf, "-----BEGIN ENCRYPTED PRIVATE KEY-----\n")) {
				vpninfo->cert_type = CERT_TYPE_PEM;
				break;
			}
		}
		fclose(f);
		if (vpninfo->cert_type == CERT_TYPE_UNKNOWN) {
			vpn_progress(vpninfo, PRG_ERR,
				     _("Failed to identify private key type in '%s'\n"),
				     vpninfo->sslkey);
			return -EINVAL;
		}
	}

	if (vpninfo->cert_type == CERT_TYPE_TPM)
		return load_tpm_certificate(vpninfo);

	/* Standard PEM certificate */
	SSL_CTX_set_default_passwd_cb(vpninfo->https_ctx, pem_pw_cb);
	SSL_CTX_set_default_passwd_cb_userdata(vpninfo->https_ctx, vpninfo);
 again:
	if (!SSL_CTX_use_RSAPrivateKey_file(vpninfo->https_ctx, vpninfo->sslkey,
					    SSL_FILETYPE_PEM)) {
		unsigned long err = ERR_peek_error();
		
		openconnect_report_ssl_errors(vpninfo);

#ifndef EVP_F_EVP_DECRYPTFINAL_EX
#define EVP_F_EVP_DECRYPTFINAL_EX EVP_F_EVP_DECRYPTFINAL
#endif
		/* If the user fat-fingered the passphrase, try again */
		if (ERR_GET_LIB(err) == ERR_LIB_EVP &&
		    ERR_GET_FUNC(err) == EVP_F_EVP_DECRYPTFINAL_EX &&
		    ERR_GET_REASON(err) == EVP_R_BAD_DECRYPT) {
			vpn_progress(vpninfo, PRG_ERR,
				     _("Loading private key failed (wrong passphrase?)\n"));
			goto again;
		}
		
		vpn_progress(vpninfo, PRG_ERR,
			     _("Loading private key failed (see above errors)\n"));
		return -EINVAL;
	}
	return 0;
}

static int get_cert_fingerprint(struct openconnect_info *vpninfo,
				X509 *cert, const EVP_MD *type,
				char *buf)
{
	unsigned char md[EVP_MAX_MD_SIZE];
	unsigned int i, n;

	if (!X509_digest(cert, type, md, &n))
		return -ENOMEM;

	for (i=0; i < n; i++)
		sprintf(&buf[i*2], "%02X", md[i]);

	return 0;
}

int get_cert_md5_fingerprint(struct openconnect_info *vpninfo,
			     X509 *cert, char *buf)
{
	return get_cert_fingerprint(vpninfo, cert, EVP_md5(), buf);
}

int openconnect_get_cert_sha1(struct openconnect_info *vpninfo,
			      X509 *cert, char *buf)
{
	return get_cert_fingerprint(vpninfo, cert, EVP_sha1(), buf);
}

static int check_server_cert(struct openconnect_info *vpninfo, X509 *cert)
{
	char fingerprint[EVP_MAX_MD_SIZE * 2 + 1];
	int ret;

	ret = openconnect_get_cert_sha1(vpninfo, cert, fingerprint);
	if (ret)
		return ret;

	if (strcasecmp(vpninfo->servercert, fingerprint)) {
		vpn_progress(vpninfo, PRG_ERR,
			     _("Server SSL certificate didn't match: %s\n"), fingerprint);
		return -EINVAL;
	}
	return 0;
}

static int match_hostname_elem(const char *hostname, int helem_len,
			       const char *match, int melem_len)
{
	if (!helem_len && !melem_len)
		return 0;

	if (!helem_len || !melem_len)
		return -1;


	if (match[0] == '*') {
		int i;

		for (i = 1 ; i <= helem_len; i++) {
			if (!match_hostname_elem(hostname + i, helem_len - i,
						 match + 1, melem_len - 1))
				return 0;
		}
		return -1;
	}

	/* From the NetBSD (5.1) man page for ctype(3):
           Values of type char or signed char must first be cast to unsigned char,
	   to ensure that the values are within the correct range.  The result
	   should then be cast to int to avoid warnings from some compilers.
	   We do indeed get warning "array subscript has type 'char'" without
	   the casts. Ick. */
	if (toupper((int)(unsigned char)hostname[0]) ==
	    toupper((int)(unsigned char)match[0]))
		return match_hostname_elem(hostname + 1, helem_len - 1,
					   match + 1, melem_len - 1);

	return -1;
}

static int match_hostname(const char *hostname, const char *match)
{
	while (*match) {
		const char *h_dot, *m_dot;
		int helem_len, melem_len;

		h_dot = strchr(hostname, '.');
		m_dot = strchr(match, '.');
		
		if (h_dot && m_dot) {
			helem_len = h_dot - hostname + 1;
			melem_len = m_dot - match + 1;
		} else if (!h_dot && !m_dot) {
			helem_len = strlen(hostname);
			melem_len = strlen(match);
		} else
			return -1;


		if (match_hostname_elem(hostname, helem_len,
					match, melem_len))
			return -1;

		hostname += helem_len;
		match += melem_len;
	}
	if (*hostname)
		return -1;

	return 0;
}

/* cf. RFC2818 and RFC2459 */
static int match_cert_hostname(struct openconnect_info *vpninfo, X509 *peer_cert)
{
	STACK_OF(GENERAL_NAME) *altnames;
	X509_NAME *subjname;
	ASN1_STRING *subjasn1;
	char *subjstr = NULL;
	int addrlen = 0;
	int i, altdns = 0;
	char addrbuf[sizeof(struct in6_addr)];
	int ret;

	/* Allow GEN_IP in the certificate only if we actually connected
	   by IP address rather than by name. */
	if (inet_pton(AF_INET, vpninfo->hostname, addrbuf) > 0)
		addrlen = 4;
	else if (inet_pton(AF_INET6, vpninfo->hostname, addrbuf) > 0)
		addrlen = 16;
	else if (vpninfo->hostname[0] == '[' &&
		 vpninfo->hostname[strlen(vpninfo->hostname)-1] == ']') {
		char *p = &vpninfo->hostname[strlen(vpninfo->hostname)-1];
		*p = 0;
		if (inet_pton(AF_INET6, vpninfo->hostname + 1, addrbuf) > 0)
			addrlen = 16;
		*p = ']';
	}

	altnames = X509_get_ext_d2i(peer_cert, NID_subject_alt_name,
				    NULL, NULL);
	for (i = 0; i < sk_GENERAL_NAME_num(altnames); i++) {
		const GENERAL_NAME *this = sk_GENERAL_NAME_value(altnames, i);

		if (this->type == GEN_DNS) {
			char *str;

			int len = ASN1_STRING_to_UTF8((void *)&str, this->d.ia5);
			if (len < 0)
				continue;

			altdns = 1;

			/* We don't like names with embedded NUL */
			if (strlen(str) != len)
				continue;

			if (!match_hostname(vpninfo->hostname, str)) {
				vpn_progress(vpninfo, PRG_TRACE,
					     _("Matched DNS altname '%s'\n"),
					     str);
				GENERAL_NAMES_free(altnames);
				OPENSSL_free(str);
				return 0;
			} else {
				vpn_progress(vpninfo, PRG_TRACE,
					     _("No match for altname '%s'\n"),
					     str);
			}
			OPENSSL_free(str);
		} else if (this->type == GEN_IPADD && addrlen) {
			char host[80];
			int family;

			if (this->d.ip->length == 4) {
				family = AF_INET;
			} else if (this->d.ip->length == 16) {
				family = AF_INET6;
			} else {
				vpn_progress(vpninfo, PRG_ERR,
					     _("Certificate has GEN_IPADD altname with bogus length %d\n"),
					     this->d.ip->length);
				continue;
			}
			
			/* We only do this for the debug messages */
			inet_ntop(family, this->d.ip->data, host, sizeof(host));

			if (this->d.ip->length == addrlen &&
			    !memcmp(addrbuf, this->d.ip->data, addrlen)) {
				vpn_progress(vpninfo, PRG_TRACE,
					     _("Matched %s address '%s'\n"),
					     (family == AF_INET6)?"IPv6":"IPv4",
					     host);
				GENERAL_NAMES_free(altnames);
				return 0;
			} else {
				vpn_progress(vpninfo, PRG_TRACE,
					     _("No match for %s address '%s'\n"),
					     (family == AF_INET6)?"IPv6":"IPv4",
					     host);
			}
		} else if (this->type == GEN_URI) {
			char *str;
			char *url_proto, *url_host, *url_path, *url_host2;
			int url_port;
			int len = ASN1_STRING_to_UTF8((void *)&str, this->d.ia5);

			if (len < 0)
				continue;

			/* We don't like names with embedded NUL */
			if (strlen(str) != len)
				continue;

			if (internal_parse_url(str, &url_proto, &url_host, &url_port, &url_path, 0)) {
				OPENSSL_free(str);
				continue;
			}

			if (!url_proto || strcasecmp(url_proto, "https"))
				goto no_uri_match;

			if (url_port != vpninfo->port)
				goto no_uri_match;

			/* Leave url_host as it was so that it can be freed */
			url_host2 = url_host;
			if (addrlen == 16 && vpninfo->hostname[0] != '[' &&
			    url_host[0] == '[' && url_host[strlen(url_host)-1] == ']') {
				/* Cope with https://[IPv6]/ when the hostname is bare IPv6 */
				url_host[strlen(url_host)-1] = 0;
				url_host2++;
			}

			if (strcasecmp(vpninfo->hostname, url_host2))
				goto no_uri_match;

			if (url_path) {
				vpn_progress(vpninfo, PRG_TRACE,
					     _("URI '%s' has non-empty path; ignoring\n"),
					     str);
				goto no_uri_match_silent;
			}
			vpn_progress(vpninfo, PRG_TRACE,
				     _("Matched URI '%s'\n"),
				     str);
			free(url_proto);
			free(url_host);
			free(url_path);
			OPENSSL_free(str);
			GENERAL_NAMES_free(altnames);
			return 0;

		no_uri_match:
			vpn_progress(vpninfo, PRG_TRACE,
				     _("No match for URI '%s'\n"),
				     str);
		no_uri_match_silent:
			free(url_proto);
			free(url_host);
			free(url_path);
			OPENSSL_free(str);
		}
	}
	GENERAL_NAMES_free(altnames);

	/* According to RFC2818, we don't use the legacy subject name if
	   there was an altname with DNS type. */
	if (altdns) {
		vpn_progress(vpninfo, PRG_ERR,
			     _("No altname in peer cert matched '%s'\n"),
			     vpninfo->hostname);
		return -EINVAL;
	}

	subjname = X509_get_subject_name(peer_cert);
	if (!subjname) {
		vpn_progress(vpninfo, PRG_ERR,
			     _("No subject name in peer cert!\n"));
		return -EINVAL;
	}

	/* Find the _last_ (most specific) commonName */
	i = -1;
	while (1) {
		int j = X509_NAME_get_index_by_NID(subjname, NID_commonName, i);
		if (j >= 0)
			i = j;
		else
			break;
	}

	subjasn1 = X509_NAME_ENTRY_get_data(X509_NAME_get_entry(subjname, i));

	i = ASN1_STRING_to_UTF8((void *)&subjstr, subjasn1);

	if (!subjstr || strlen(subjstr) != i) {
		vpn_progress(vpninfo, PRG_ERR,
			     _("Failed to parse subject name in peer cert\n"));
		return -EINVAL;
	}
	ret = 0;

	if (match_hostname(vpninfo->hostname, subjstr)) {
		vpn_progress(vpninfo, PRG_ERR,
			     _("Peer cert subject mismatch ('%s' != '%s')\n"),
			     subjstr, vpninfo->hostname);
		ret = -EINVAL;
	} else {
		vpn_progress(vpninfo, PRG_TRACE,
			     _("Matched peer certificate subject name '%s'\n"),
			     subjstr);
	}

	OPENSSL_free(subjstr);			  
	return ret;
}

static int verify_peer(struct openconnect_info *vpninfo, SSL *https_ssl)
{
	X509 *peer_cert;
	int ret;

	peer_cert = SSL_get_peer_certificate(https_ssl);

	if (vpninfo->servercert) {
		/* If given a cert fingerprint on the command line, that's
		   all we look for */
		ret = check_server_cert(vpninfo, peer_cert);
	} else {
		int vfy = SSL_get_verify_result(https_ssl);
		const char *err_string = NULL;

		if (vfy != X509_V_OK)
			err_string = X509_verify_cert_error_string(vfy);
		else if (match_cert_hostname(vpninfo, peer_cert))
			err_string = _("certificate does not match hostname");

		if (err_string) {
			vpn_progress(vpninfo, PRG_INFO,
				     _("Server certificate verify failed: %s\n"),
				     err_string);

			if (vpninfo->validate_peer_cert)
				ret = vpninfo->validate_peer_cert(vpninfo->cbdata,
								  peer_cert,
								  err_string);
			else
				ret = -EINVAL;
		} else {
			ret = 0;
		}
	}
	X509_free(peer_cert);

	return ret;
}

static void workaround_openssl_certchain_bug(struct openconnect_info *vpninfo,
					     SSL *ssl)
{
	/* OpenSSL has problems with certificate chains -- if there are
	   multiple certs with the same name, it doesn't necessarily
	   choose the _right_ one. (RT#1942)
	   Pick the right ones for ourselves and add them manually. */
	X509 *cert = SSL_get_certificate(ssl);
	X509 *cert2;
	X509_STORE *store = SSL_CTX_get_cert_store(vpninfo->https_ctx);
	X509_STORE_CTX ctx;

	if (!cert || !store)
		return;

	/* If we already have 'supporting' certs, don't add them again */
	if (vpninfo->https_ctx->extra_certs)
		return;

	if (!X509_STORE_CTX_init(&ctx, store, NULL, NULL))
		return;

	while (ctx.get_issuer(&cert2, &ctx, cert) == 1) {
		char buf[200];
		if (cert2 == cert)
			break;
		if (X509_check_issued(cert2, cert2) == X509_V_OK)
			break;
		cert = cert2;
		X509_NAME_oneline(X509_get_subject_name(cert),
				  buf, sizeof(buf));
		vpn_progress(vpninfo, PRG_DEBUG,
			     _("Extra cert from cafile: '%s'\n"), buf);
		SSL_CTX_add_extra_chain_cert(vpninfo->https_ctx, cert);
	}
	X509_STORE_CTX_cleanup(&ctx);
}

#if OPENSSL_VERSION_NUMBER >= 0x00908000
static int ssl_app_verify_callback(X509_STORE_CTX *ctx, void *arg)
{
	/* We've seen certificates in the wild which don't have the
	   purpose fields filled in correctly */
	X509_VERIFY_PARAM_set_purpose(ctx->param, X509_PURPOSE_ANY);
	return X509_verify_cert(ctx);
}
#endif

static int check_certificate_expiry(struct openconnect_info *vpninfo)
{
	ASN1_TIME *notAfter;
	const char *reason = NULL;
	time_t t;
	int i;

	if (!vpninfo->cert_x509)
		return 0;

	t = time(NULL);
	notAfter = X509_get_notAfter(vpninfo->cert_x509);
	i = X509_cmp_time(notAfter, &t);
	if (!i) {
		vpn_progress(vpninfo, PRG_ERR,
			     _("Error in client cert notAfter field\n"));
		return -EINVAL;
	} else if (i < 0) {
		reason = _("Client certificate has expired at");
	} else {
		t += vpninfo->cert_expire_warning;
		i = X509_cmp_time(notAfter, &t);
		if (i < 0) {
			reason = _("Client certificate expires soon at");
		}
	}
	if (reason) {
		BIO *bp = BIO_new(BIO_s_mem());
		BUF_MEM *bm;
		const char *expiry = _("<error>");
		char zero = 0;

		if (bp) {
			ASN1_TIME_print(bp, notAfter);
			BIO_write(bp, &zero, 1);
			BIO_get_mem_ptr(bp, &bm);
			expiry = bm->data;
		}
		vpn_progress(vpninfo, PRG_ERR, "%s: %s\n", reason, expiry);
		if (bp)
			BIO_free(bp);
	}
	return 0;
}
int openconnect_open_https(struct openconnect_info *vpninfo)
{
	method_const SSL_METHOD *ssl3_method;
	SSL *https_ssl;
	BIO *https_bio;
	int ssl_sock;
	int err;

	if (vpninfo->https_ssl)
		return 0;

	if (vpninfo->peer_cert) {
		X509_free(vpninfo->peer_cert);
		vpninfo->peer_cert = NULL;
	}

	ssl_sock = connect_https_socket(vpninfo);
	if (ssl_sock < 0)
		return ssl_sock;

	ssl3_method = TLSv1_client_method();
	if (!vpninfo->https_ctx) {
		vpninfo->https_ctx = SSL_CTX_new(ssl3_method);

		/* Some servers (or their firewalls) really don't like seeing
		   extensions. */
#ifdef SSL_OP_NO_TICKET
		SSL_CTX_set_options (vpninfo->https_ctx, SSL_OP_NO_TICKET);
#endif

		if (vpninfo->cert) {
			err = load_certificate(vpninfo);
			if (err) {
				vpn_progress(vpninfo, PRG_ERR,
					     _("Loading certificate failed. Aborting.\n"));
				return err;
			}
			check_certificate_expiry(vpninfo);
		}

		/* We just want to do:
		   SSL_CTX_set_purpose(vpninfo->https_ctx, X509_PURPOSE_ANY); 
		   ... but it doesn't work with OpenSSL < 0.9.8k because of 
		   problems with inheritance (fixed in v1.1.4.6 of
		   crypto/x509/x509_vpm.c) so we have to play silly buggers
		   instead. This trick doesn't work _either_ in < 0.9.7 but
		   I don't know of _any_ workaround which will, and can't
		   be bothered to find out either. */
#if OPENSSL_VERSION_NUMBER >= 0x00908000
		SSL_CTX_set_cert_verify_callback(vpninfo->https_ctx,
						 ssl_app_verify_callback, NULL);
#endif
		SSL_CTX_set_default_verify_paths(vpninfo->https_ctx);

		if (vpninfo->cafile) {
			if (!SSL_CTX_load_verify_locations(vpninfo->https_ctx, vpninfo->cafile, NULL)) {
				vpn_progress(vpninfo, PRG_ERR,
					     _("Failed to open CA file '%s'\n"),
					     vpninfo->cafile);
				openconnect_report_ssl_errors(vpninfo);
				close(ssl_sock);
				return -EINVAL;
			}
		}

	}
	https_ssl = SSL_new(vpninfo->https_ctx);
	workaround_openssl_certchain_bug(vpninfo, https_ssl);

	https_bio = BIO_new_socket(ssl_sock, BIO_NOCLOSE);
	BIO_set_nbio(https_bio, 1);
	SSL_set_bio(https_ssl, https_bio, https_bio);

	vpn_progress(vpninfo, PRG_INFO, _("SSL negotiation with %s\n"),
		     vpninfo->hostname);

	while ((err = SSL_connect(https_ssl)) <= 0) {
		fd_set wr_set, rd_set;
		int maxfd = ssl_sock;
		
		FD_ZERO(&wr_set);
		FD_ZERO(&rd_set);

		err = SSL_get_error(https_ssl, err);
		if (err == SSL_ERROR_WANT_READ)
			FD_SET(ssl_sock, &rd_set);
		else if (err == SSL_ERROR_WANT_WRITE)
			FD_SET(ssl_sock, &wr_set);
		else {
			vpn_progress(vpninfo, PRG_ERR, _("SSL connection failure\n"));
			openconnect_report_ssl_errors(vpninfo);
			SSL_free(https_ssl);
			close(ssl_sock);
			return -EINVAL;
		}

		if (vpninfo->cancel_fd != -1) {
			FD_SET(vpninfo->cancel_fd, &rd_set);
			if (vpninfo->cancel_fd > ssl_sock)
				maxfd = vpninfo->cancel_fd;
		}
		select(maxfd + 1, &rd_set, &wr_set, NULL, NULL);
		if (vpninfo->cancel_fd != -1 &&
		    FD_ISSET(vpninfo->cancel_fd, &rd_set)) {
			vpn_progress(vpninfo, PRG_ERR, _("SSL connection cancelled\n"));
			SSL_free(https_ssl);
			close(ssl_sock);
			return -EINVAL;
		}
	}

	if (verify_peer(vpninfo, https_ssl)) {
		SSL_free(https_ssl);
		close(ssl_sock);
		return -EINVAL;
	}

	vpninfo->ssl_fd = ssl_sock;
	vpninfo->https_ssl = https_ssl;

	/* Stash this now, because it might not be available later if the
	   server has disconnected. */
	vpninfo->peer_cert = SSL_get_peer_certificate(vpninfo->https_ssl);

	vpn_progress(vpninfo, PRG_INFO, _("Connected to HTTPS on %s\n"),
		     vpninfo->hostname);

	return 0;
}

void openconnect_close_https(struct openconnect_info *vpninfo)
{
	if (vpninfo->peer_cert) {
		X509_free(vpninfo->peer_cert);
		vpninfo->peer_cert = NULL;
	}
	if (vpninfo->https_ssl) {
		SSL_free(vpninfo->https_ssl);
		vpninfo->https_ssl = NULL;
	}
	if (vpninfo->ssl_fd != -1) {
		close(vpninfo->ssl_fd);
		FD_CLR(vpninfo->ssl_fd, &vpninfo->select_rfds);
		FD_CLR(vpninfo->ssl_fd, &vpninfo->select_wfds);
		FD_CLR(vpninfo->ssl_fd, &vpninfo->select_efds);
		vpninfo->ssl_fd = -1;
	}
}

void openconnect_init_openssl(void)
{
	SSL_library_init ();
	ERR_clear_error ();
	SSL_load_error_strings ();
	OpenSSL_add_all_algorithms ();
}

char *openconnect_get_cert_details(struct openconnect_info *vpninfo,
				   struct x509_st *cert)
{
	BIO *bp = BIO_new(BIO_s_mem());
	BUF_MEM *certinfo;
	char zero = 0;
	char *ret;

	X509_print_ex(bp, cert, 0, 0);
	BIO_write(bp, &zero, 1);
	BIO_get_mem_ptr(bp, &certinfo);

	ret = strdup(certinfo->data);
	BIO_free(bp);
	return ret;
}


int openconnect_local_cert_md5(struct openconnect_info *vpninfo,
			       char *buf)
{
	buf[0] = 0;

	if (!vpninfo->cert_x509)
		return -EIO;

	if (get_cert_md5_fingerprint(vpninfo, vpninfo->cert_x509, buf))
		return -EIO;

	return 0;
}
