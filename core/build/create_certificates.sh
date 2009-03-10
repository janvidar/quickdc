#!/bin/sh

QDCHOME=~/.quickdc
CID=QuickDC
OPENSSL=/usr/bin/openssl
GNUTLS=/usr/bin/certtool

MODE=OPENSSL

CERT_EXPIRE_DAYS=3650

mkdir -p  ${QDCHOME}/ssl/


if [ "${MODE}" = "OPENSSL" ]; then
	PRIVKEY=${QDCHOME}/ssl/openssl.key
	PUBCERT=${QDCHOME}/ssl/openssl.crt
	if [ -x ${OPENSSL} ]; then
		${OPENSSL} genrsa -out ${PRIVKEY} 2048
		${OPENSSL} req -x509 -new -batch -days ${CERT_EXPIRE_DAYS} -subj "/CN=${CID}/" -key ${PRIVKEY} -out ${PUBCERT}
	else
		echo OpenSSL not found on your system
	fi
	exit
fi

if [ "${MODE}" = "GNUTLS" ]; then
	PRIVKEY=${QDCHOME}/ssl/gnutls.key
	PUBCERT=${QDCHOME}/ssl/gnutls.crt
	TEMPLATE=${QDCHOME}/ssl/gnutls_template

	if [ -x ${GNUTLS} ]; then
		# ${GNUTLS} --generate-privkey --bits 2048 --outfile ${PRIVKEY}
		echo cn="${CID}" > ${TEMPLATE}
		echo expiration_days=${CERT_EXPIRE_DAYS} >> ${TEMPLATE}
		echo ${GNUTLS} --generate-self-signed --load-privkey ${PRIVKEY} --outfile ${PUBCERT} --template ${TEMPLATE}
	else
		echo GNUTLS not found on your system
	fi
	exit
fi

echo "No mode"
