"""Ensure WiGLE retains certificate verification and avoids MFL negotiation."""
from pathlib import Path
root=Path(__file__).resolve().parents[1]
s=(root/'main/wigle.c').read_text()
c=(root/'sdkconfig').read_text()
d=(root/'sdkconfig.defaults').read_text()
assert s.count('.crt_bundle_attach = esp_crt_bundle_attach') == 2
assert 'mbedtls_ssl_conf_max_frag_len' not in s
assert '# CONFIG_MBEDTLS_SSL_KEEP_PEER_CERTIFICATE is not set' in c
assert '# CONFIG_MBEDTLS_SSL_KEEP_PEER_CERTIFICATE is not set' in d
assert 'CONFIG_MBEDTLS_SSL_IN_CONTENT_LEN=16384' in c
print('WiGLE TLS: certificate validation retained, no forced fragmentation, peer certificate released')
