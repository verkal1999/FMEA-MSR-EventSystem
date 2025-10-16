# certificates/

Example client certificate and private key for OPC UA **Sign&Encrypt** sessions.

> Replace these with your own credentials for any non-test environment. Never commit production private keys.

## Generate example credentials (OpenSSL)
```bash
# Create a client cert/key (adjust subject, SANs, and validity as needed)
openssl req -x509 -newkey rsa:2048 -nodes \
  -keyout client_key.pem -out client_cert.pem -days 365 \
  -subj "/CN=FMEA-MSR-EventSystem Client"

# Convert to DER if your stack prefers it
openssl x509 -in client_cert.pem -outform der -out client_cert.der
openssl pkey -in client_key.pem -outform der -out client_key.der
