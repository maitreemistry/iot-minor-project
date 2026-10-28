#pragma once

// Download the official CA certificate for broker.emqx.io from:
// https://assets.emqx.com/data/broker.emqx.io-ca.crt
//
// Open the downloaded .crt file in a text editor and replace the placeholder
// below with the ENTIRE PEM contents, including BEGIN/END CERTIFICATE lines.
// Keep the \n line breaks exactly as they appear in the downloaded file.
//
// Do not use espClient.setInsecure(): it disables certificate verification.

static const char EMQX_CA_CERT[] PROGMEM = R"EMQXCA(
PASTE THE FULL CONTENTS OF broker.emqx.io-ca.crt HERE
)EMQXCA";
