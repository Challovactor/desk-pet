/*
 * 服务器 CA 证书（PEM），来自 server/key.txt / ca.crt。
 * 公开证书，不是私钥；desk-recorder/v1 的 HTTPS/MQTTS 用它校验服务器。
 */
#include "app_ca_cert.h"

#include <string.h>

static const char s_ca_pem[] =
    "-----BEGIN CERTIFICATE-----\n"
    "MIIEOjCCAqKgAwIBAgIURrPOUOeGhSGxh9LTqSc49dal4SQwDQYJKoZIhvcNAQEL\n"
    "BQAwIzEhMB8GA1UEAwwYWElBTyBSZWNvcmRlciBQcml2YXRlIENBMB4XDTI2MTEx\n"
    "NDEyMzEwMVoXDTM2MTExMTEyMzEwMVowIzEhMB8GA1UEAwwYWElBTyBSZWNvcmRl\n"
    "ciBQcml2YXRlIENBMIIBojANBgkqhkiG9w0BAQEFAAOCAY8AMIIBigKCAYEAtWOy\n"
    "jAn2lSvNTELp1zG6pFUsZVJImKGDLBdQIP9mTIbzaLVxeOV0yinozSnyjg7KXuCx\n"
    "SItui/+ZR3FQboIpxto2wlVGDA61jxthqm6c2zZUF1JfT6KX945qTBt9fSNu9x/f\n"
    "6dy7Wshzduh1MMq9VHV0D4oncp103dEHRf3yeJ4/XBlMV/SAcvvXiZLcjk7iYoKF\n"
    "KNOq411C4wFjZl45Ct/+ZvPTTTBC0JWpXEAwur2Uw/ufGVPFOPELnhaCHqhdx3n8\n"
    "0tNaI0lfa4KvclXnKcz85n0f0Buyz+APZSgXg9QFhDhtaDeQkLON4PJOmxWHEpmd\n"
    "S9aZ0CH1hEESz1vaElKU5D4TKdMLR6qxJQMQZDFBqDaMpcmroFzeBJ2jP5bVwHQ5\n"
    "5/KqvqixY7FHQGqd6lEzzLq+xXMCBiRYYlufhZOdn32dg9QWYp+sUWycMoD2P+7K\n"
    "g0fasFG3ZjOcB7xHzmBJfLX3nr7PZLGHggp73VgltSpQxFtMHo2CCV8oF4obAgMB\n"
    "AAGjZjBkMB0GA1UdDgQWBBT1aJKq7f1Ly6G0gQ5nRrkFRENU7jAfBgNVHSMEGDAW\n"
    "gBT1aJKq7f1Ly6G0gQ5nRrkFRENU7jASBgNVHRMBAf8ECDAGAQH/AgEAMA4GA1Ud\n"
    "DwEB/wQEAwIBBjANBgkqhkiG9w0BAQsFAAOCAYEAWYVDA6s9gLnVOOizVQgY7daM\n"
    "Gbi+5w/In6QqPvT3Jh9cHb367pGwaD0Df+QeUhsQDGK1vQnsRbmNWTOjKgZ3SK2b\n"
    "EcL0kcqV0l1+qWwgf7yOqeZ+A8FbxgIFCwSJtJt+9jZwcrpBIg2XVbe8WfiolNZO\n"
    "g0DoiICUgBaMCA24/y7PuxvP+Jshy0wIioKMQTzR/o1yFGvZqiSXs0kw86GWqvUL\n"
    "S/ctWPD7kgtzN29tnunlMqiW4tWMGGHd7h7iiqNTTWVHo2rJTPMoKtiOQ9nFKNyt\n"
    "td2EhS3IjNlowNeRkpe5Zm9iB0Q6PypWcs7mY/WXquw1VhmXqSI4Oxp9PbZ5MNna\n"
    "FTNjwcd69cfGvavClHk+glEjGXr6oolgKDnuiFZAP9iQ4uUMZeKdSqldQ19oNfK1\n"
    "MOyHX3rehSc6G6+dO0RNKQtj11mnDPQZ1PahZKBqRUUtt80jNZCe9Asd163jl8TD\n"
    "xY0vrioiXjq7oQX+IELccJqCyZmvb7JJV4uQS5p1\n"
    "-----END CERTIFICATE-----\n"
;

const char *app_ca_pem(void)
{
    return s_ca_pem;
}

int app_ca_is_placeholder(void)
{
    return strstr(s_ca_pem, "REPLACE_WITH_SERVER_CA_PEM") != NULL;
}
