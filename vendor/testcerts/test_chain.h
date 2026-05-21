/* vendor/testcerts/test_chain.h -- chapter 112b in-guest TLS test cert.
 *
 * Re-exports BearSSL 0.6's sample RSA-2048 self-signed cert
 * chain for CN=localhost (vendor/bearssl/samples/chain-rsa.h
 * and key-rsa.h, MIT-licensed by Thomas Pornin).  Used by:
 *
 *   - userspace/httpsd/httpsd.c   (server side: presents the chain)
 *   - userspace/tlstest/tlstest.c (client side: pins on CERT0's
 *     public key via br_x509_knownkey for the 112b regression)
 *
 * Chapter 112c will introduce real chain validation against a
 * root CA store; until then "trust" reduces to "the client knows
 * the server's public key bytes ahead of time".
 */
#ifndef OSDEV_VENDOR_TESTCERTS_TEST_CHAIN_H
#define OSDEV_VENDOR_TESTCERTS_TEST_CHAIN_H

#include "bearssl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Server certificate chain (end-entity, intermediate).  The
 * chain validates as long as the verifier trusts the intermediate
 * cert (which we don't in 112b -- we use knownkey instead). */
extern const br_x509_certificate *const test_server_chain;
extern const size_t                     test_server_chain_len;

/* Server RSA-2048 private key, in CRT form. */
extern const br_rsa_private_key  *const test_server_key;

/* Chapter 112e: a second sample chain (ECDSA / P-256) re-exported
 * from vendor/testcerts/test_chain_ec.c.  Lets a single httpsd
 * binary present either chain at runtime (selected by CLI flag)
 * and lets the browser bundle two trust anchors so the chapter-
 * 112e multi-anchor store is genuinely exercised. */
extern const br_x509_certificate *const test_server_chain_ec;
extern const size_t                     test_server_chain_ec_len;
extern const br_ec_private_key   *const test_server_key_ec;

#ifdef __cplusplus
}
#endif

#endif
