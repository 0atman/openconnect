# What is this?

This is a modified version of the fantastic open-source VPN client
[OpenConnect](https://infradead.org/openconnect) which supports the
PAN GlobalProtect VPN in its native modes (SSL and
[ESP](http://wikipedia.org/wiki/Encapsulating_Security_Payload))—with
no assistance or cooperation needed from your VPN administrators.

This is a [work in progress](http://lists.infradead.org/pipermail/openconnect-devel/2016-October/004035.html),
but I've been using it for real work already and it works very well
for me.

Having other people test it would be awesome and I welcome your
feedback! Please report any problems here on Github rather than
bothering the OpenConnect mailing list, since this is *not part of any
official OpenConnect release*.

## Installation

Build the `globalprotect` branch from this repository. Requires modern versions of `autoconf`, `automake`, `gcc`, `libxml`.

```sh
$ git clone git@github.com:dlenski/openconnect
$ cd openconnect
$ ./autogen.sh
$ ./configure
$ make
```

## Connecting

Run openconnect like this to test it with your GlobalProtect VPN
provider. (Include `--certificate cert_with_privkey.pem` if your VPN
requires a client certificate and/or private key.)

```sh
$ ./openconnect --protocol=gp server.company.com --dump -vvv
Please enter your username and password.
Username:
Password:
```

Currently it only supports username, password, and optionally client
certificate authentication… since that's the only example I have. But
I'd welcome feedback if there are other authentication methods in use
out there.

## Portal vs. gateway servers

For my VPN, the VPN tunnel server is the *same* as the VPN "portal"
server, but your VPN may differ. Try using both the "Portal address"
and the "GlobalProtect Gateway IP" shown in the Windows client with
OpenConnect:

[![GlobalProtect Windows client](https://i.stack.imgur.com/2JC9T.png)]
