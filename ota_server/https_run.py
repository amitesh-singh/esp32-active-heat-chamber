import http.server
import ssl

PORT = 443

handler = http.server.SimpleHTTPRequestHandler
httpd = http.server.HTTPServer(("0.0.0.0", PORT), handler)

# Create SSL context
context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
context.load_cert_chain(certfile="cert.pem", keyfile="key.pem")

# Wrap socket
httpd.socket = context.wrap_socket(httpd.socket, server_side=True)

print(f"Serving HTTPS on port {PORT}")
httpd.serve_forever()
