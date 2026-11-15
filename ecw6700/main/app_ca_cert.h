#ifndef APP_CA_CERT_H
#define APP_CA_CERT_H

/* 服务器安装程序生成的 CA 证书（PEM）。部署时替换 app_ca_cert.c 的内容。
 * 返回的字符串必须 NUL 结尾；app_ca_is_placeholder() 用于在启动日志里
 * 明确提示“还没换证书”，避免把 TLS 失败误判成网络问题。 */
const char *app_ca_pem(void);
int app_ca_is_placeholder(void);

#endif
