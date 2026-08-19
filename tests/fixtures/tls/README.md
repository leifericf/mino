# TLS test fixtures

Throwaway certificate material for tests/tls_test.clj. Nothing here
is secret and nothing here should ever be trusted outside the test
suite; the CA is unknown to the vendored Mozilla root store, which is
the point (the default verification path must refuse it).

Files:

- `ca.pem` / `ca.key` -- test CA (RSA 2048, CN "Mino Test CA")
- `server.pem` / `server.key` -- CN "localhost", SAN
  DNS:localhost + IP:127.0.0.1, signed by the test CA
- `wrong-host.pem` / `wrong-host.key` -- CN/SAN "other.example", signed
  by the test CA (SNI hostname mismatch)
- `expired.pem` / `expired.key` -- self-signed, CN "localhost", SAN
  DNS:localhost + IP:127.0.0.1, valid 2025-01-01..2026-01-01 (expired)

Regenerate with the exact commands below (OpenSSL 3.x; `-not_before` /
`-not_after` need 3.3+):

```sh
cd tests/fixtures/tls

openssl req -x509 -newkey rsa:2048 -nodes -keyout ca.key -out ca.pem \
  -days 3650 -sha256 -subj "/O=Mino Tests/CN=Mino Test CA" \
  -addext "basicConstraints=critical,CA:TRUE" \
  -addext "keyUsage=critical,keyCertSign,cRLSign"

printf 'subjectAltName=DNS:localhost,IP:127.0.0.1\nbasicConstraints=CA:FALSE\nkeyUsage=digitalSignature,keyEncipherment\nextendedKeyUsage=serverAuth\n' > server.ext
openssl req -newkey rsa:2048 -nodes -keyout server.key -out server.csr \
  -sha256 -subj "/O=Mino Tests/CN=localhost"
openssl x509 -req -in server.csr -CA ca.pem -CAkey ca.key \
  -CAcreateserial -out server.pem -days 825 -sha256 -extfile server.ext

printf 'subjectAltName=DNS:other.example\nbasicConstraints=CA:FALSE\nkeyUsage=digitalSignature,keyEncipherment\nextendedKeyUsage=serverAuth\n' > wrong-host.ext
openssl req -newkey rsa:2048 -nodes -keyout wrong-host.key \
  -out wrong-host.csr -sha256 -subj "/O=Mino Tests/CN=other.example"
openssl x509 -req -in wrong-host.csr -CA ca.pem -CAkey ca.key \
  -CAcreateserial -out wrong-host.pem -days 825 -sha256 \
  -extfile wrong-host.ext

printf 'subjectAltName=DNS:localhost,IP:127.0.0.1\nbasicConstraints=critical,CA:TRUE\nkeyUsage=digitalSignature,keyEncipherment,keyCertSign\n' > expired.ext
openssl req -newkey rsa:2048 -nodes -keyout expired.key -out expired.csr \
  -sha256 -subj "/O=Mino Tests/CN=localhost"
openssl x509 -req -in expired.csr -signkey expired.key -out expired.pem \
  -sha256 -not_before 20250101000000Z -not_after 20260101000000Z \
  -extfile expired.ext

rm -f server.csr wrong-host.csr expired.csr server.ext wrong-host.ext \
  expired.ext ca.srl
```
