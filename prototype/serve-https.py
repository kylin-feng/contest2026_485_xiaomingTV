#!/usr/bin/env python3
"""sleep-sim HTTPS 静态服务（手机语音需要安全上下文）

用法：
    python3 serve-https.py            # 证书缺失时自动生成（需 openssl），然后起服务
    python3 serve-https.py --cn 192.168.0.174   # 指定证书里的局域网 IP（默认自动探测）

证书存放在本脚本同目录（sle-cert.pem / sle-key.pem），不再依赖 /tmp（Windows 没有）。
首次在手机上访问 https://<局域网IP>:8443/m.html 需手动信任自签证书。
"""
import http.server, ssl, os, socket, subprocess, sys, argparse

HERE = os.path.dirname(os.path.abspath(__file__))
CERT = os.path.join(HERE, 'sle-cert.pem')
KEY = os.path.join(HERE, 'sle-key.pem')

def lan_ip():
    """探测本机局域网 IP（连不上外网时返回 127.0.0.1）"""
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect(('8.8.8.8', 80)); return s.getsockname()[0]
    except OSError:
        return '127.0.0.1'
    finally:
        s.close()

def gen_cert(cn):
    print(f'证书缺失，用 openssl 生成（CN={cn}）……')
    r = subprocess.run(['openssl', 'req', '-x509', '-newkey', 'rsa:2048',
        '-keyout', KEY, '-out', CERT, '-days', '365', '-nodes',
        '-subj', f'/CN={cn}',
        '-addext', f'subjectAltName=IP:{cn},IP:127.0.0.1,DNS:localhost'], cwd=HERE)
    if r.returncode != 0:
        sys.exit('openssl 生成证书失败。可手动生成后放到本目录：\n'
                 f'  openssl req -x509 -newkey rsa:2048 -keyout {KEY} -out {CERT} '
                 f'-days 365 -nodes -subj "/CN={cn}" -addext "subjectAltName=IP:{cn},IP:127.0.0.1,DNS:localhost"')

if __name__ == '__main__':
    ap = argparse.ArgumentParser()
    ap.add_argument('--cn', default=None, help='证书 CN / 局域网 IP（默认自动探测）')
    ap.add_argument('--port', type=int, default=8443)
    a = ap.parse_args()

    if not (os.path.exists(CERT) and os.path.exists(KEY)):
        gen_cert(a.cn or lan_ip())

    os.chdir(HERE)
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    ctx.load_cert_chain(CERT, KEY)
    srv = http.server.ThreadingHTTPServer(('0.0.0.0', a.port), http.server.SimpleHTTPRequestHandler)
    srv.socket = ctx.wrap_socket(srv.socket, server_side=True)
    print(f'https://{lan_ip()}:{a.port}/m.html  （手机与电脑需同一 WiFi，首次访问需信任证书）')
    srv.serve_forever()
