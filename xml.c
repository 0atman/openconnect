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

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h> 
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <libxml/parser.h>
#include <libxml/tree.h>

#include "anyconnect.h"

int config_lookup_host(struct anyconnect_info *vpninfo, const char *host)
{
	int fd, i;
	struct stat st;
	char *xmlfile;
	EVP_MD_CTX c;
	unsigned char sha1[SHA_DIGEST_LENGTH];
	xmlDocPtr xml_doc;
	xmlNode *xml_node, *xml_node2;
	
	if (!vpninfo->xmlconfig) {
		vpninfo->hostname = host;
		return 0;
	}

	fd = open(vpninfo->xmlconfig, O_RDONLY);
	if (fd < 0) {
		perror("Open XML config file");
		fprintf(stderr, "Treating host \"%s\" as a raw hostname\n", host);
		vpninfo->hostname = host;
		return 0;
	}

	if (fstat(fd, &st)) {
		perror("fstat XML config file");
		return -1;
	}

	xmlfile = malloc(st.st_size);
	if (!xmlfile) {
		fprintf(stderr, "Could not allocate %zd bytes for XML config file\n", st.st_size);
		close(fd);
		return -1;
	}

	xmlfile = mmap(NULL, st.st_size, PROT_READ, MAP_SHARED, fd, 0);
	if (xmlfile == MAP_FAILED) {
		perror("mmap XML config file");
		close(fd);
		return -1;
	}

	EVP_MD_CTX_init(&c);
	EVP_Digest(xmlfile, st.st_size, sha1, NULL, EVP_sha1(), NULL);
	EVP_MD_CTX_cleanup(&c);

	for (i = 0; i < SHA_DIGEST_LENGTH; i++)
		sprintf(&vpninfo->xmlsha1[i*2], "%02x", sha1[i]);

	if (verbose)
		printf("XML config file SHA1: %s\n", vpninfo->xmlsha1);

	xml_doc = xmlReadMemory(xmlfile, st.st_size, "noname.xml", NULL, 0);
	if (!xml_doc) {
		fprintf(stderr, "Failed to parse XML config file %s\n", vpninfo->xmlconfig);
		fprintf(stderr, "Treating host \"%s\" as a raw hostname\n", host);
		vpninfo->hostname = host;
		return 0;
	}
	xml_node = xmlDocGetRootElement(xml_doc);

	for (xml_node = xml_node->children; xml_node; xml_node = xml_node->next) {
		if (xml_node->type == XML_ELEMENT_NODE &&
		    !strcmp((char *)xml_node->name, "ServerList")) {

			for (xml_node = xml_node->children; xml_node && !vpninfo->hostname;
			     xml_node = xml_node->next) {

				if (xml_node->type == XML_ELEMENT_NODE &&
				    !strcmp((char *)xml_node->name, "HostEntry")) {
					int match = 0;

					for (xml_node2 = xml_node->children;
					     match >= 0 && xml_node2; xml_node2 = xml_node2->next) {

						if (xml_node2->type != XML_ELEMENT_NODE)
							continue;

						if (!match && !strcmp((char *)xml_node2->name, "HostName")) {
							char *content = (char *)xmlNodeGetContent(xml_node2);
							if (content && !strcmp(content, host))
								match = 1;
							else
								match = -1;
						} else if (match &&
							   !strcmp((char *)xml_node2->name, "HostAddress")) {
							char *content = (char *)xmlNodeGetContent(xml_node2);
							if (content) {
								vpninfo->hostname = strdup(content);
								printf("Host \"%s\" has address \"%s\"\n",
								       host, content);
								break;
							}
						}
					}
				}

			}
			break;
		}
	}
	xmlFreeDoc(xml_doc);

	if (!vpninfo->hostname) {
		fprintf(stderr, "Host \"%s\" not listed in config; treating as raw hostname\n",
			host);
		vpninfo->hostname = host;
	}
		
	return 0;
}
