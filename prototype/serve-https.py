#!/usr/bin/env python3
"""sleep-sim HTTPS 静态服务（手机语音需要安全上下文）"""
import http.server, ssl, os
os.chdir(os.path.dirname(os.path.abspath(__file__)))
ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
ctx.load_cert_chain('/tmp/sle-cert.pem', '/tmp/sle-key.pem')
srv = http.server.ThreadingHTTPServer(('0.0.0.0', 8443), http.server.SimpleHTTPRequestHandler)
srv.socket = ctx.wrap_socket(srv.socket, server_side=True)
print('https://0.0.0.0:8443')
srv.serve_forever()
