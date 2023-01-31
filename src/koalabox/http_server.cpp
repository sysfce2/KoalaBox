#include <koalabox/globals.hpp>
#include <koalabox/http_server.hpp>
#include <koalabox/paths.hpp>
#include <koalabox/util.hpp>

#include <openssl/pem.h>
#include <openssl/x509.h>
#include <koalabox/logger.hpp>

namespace koalabox::http_server {

    // Source: https://gist.github.com/nathan-osman/5041136
    class Certificate {
    private:
        EVP_PKEY* key = nullptr;
        X509* x509 = nullptr;

        void free_key() {
            EVP_PKEY_free(key);
            key = nullptr;
        }

        void free_cert() {
            X509_free(x509);
            x509 = nullptr;

            free_key();
        }

        void create_key() {
            LOG_DEBUG("Creating an RSA key")

            // Allocate memory for the EVP_PKEY structure.
            key = EVP_PKEY_new();

            if (!key) {
                throw Exception("Unable to create an EVP_PKEY structure.");
            }

            // Generate the RSA key and assign it to pkey
            RSA* rsa = RSA_generate_key(2048, RSA_F4, NULL, NULL);

            if (!EVP_PKEY_assign_RSA(key, rsa)) {
                EVP_PKEY_free(key);
                throw Exception("Unable to generate an RSA key.");
            }

            // The key has been generated at this point
            LOG_DEBUG("Successfully created an RSA key")
        }

        Certificate(
            const String& common_name,
            const Certificate& issuer,
            const Map<int, String> ext_map // See https://superuser.com/a/1248085
        ) {
            create_key();

            LOG_DEBUG("Creating an x509 certificate")

            // Allocate memory for the X509 structure.
            x509 = X509_new();

            if (not x509) {
                throw Exception("Unable to create an X509 structure.");
            }

            X509_set_version(x509, 2);

            // Create random serial
            // Source: https://github.com/pashapovalov/Webinos-Platform-buildhive/blob/386178ed83198c7b802aa894f579899edf199525/webinos/common/manager/certificate_manager/src/openssl_wrapper.cpp#L166-L176
            ASN1_INTEGER* serial = ASN1_INTEGER_new();
            BIGNUM* big_num = BN_new();
            //64 bits of randomness?
            BN_pseudo_rand(big_num, 120, 0, 0);
            BN_to_ASN1_INTEGER(big_num, serial);
            BN_free(big_num);
            X509_set_serialNumber(x509, serial);

            // This certificate is valid from now until 25 year from now.
            X509_gmtime_adj(X509_get_notBefore(x509), 0);
            X509_gmtime_adj(X509_get_notAfter(x509), 25 * 365 * 24 * 60 * 60);

            // Set the public key for our certificate.
            X509_set_pubkey(x509, key);

            // We want to copy the subject name to the issuer name.
            X509_NAME* subject_name = X509_get_subject_name(x509);

            const auto add_entry = [&](const String& field, const String& data) {
                X509_NAME_add_entry_by_txt(
                    subject_name, // name
                    field.c_str(), // field
                    MBSTRING_ASC, // type
                    (unsigned char*) data.c_str(), // bytes
                    -1, // len
                    -1, // loc
                    0 // set
                );
            };

            // Set the country code and common name.
            const String project_name = koalabox::globals::get_project_name();
            add_entry("O", "acidicoala");
            add_entry("CN", common_name);

            for (const auto& [nid, data]: ext_map) {
                X509V3_CTX ctx;
                X509V3_set_ctx_nodb(&ctx)

                X509V3_set_ctx(&ctx, issuer.x509, x509, NULL, NULL, 0);
                auto* ext = X509V3_EXT_conf_nid(NULL, &ctx, nid, data.c_str());
                if (not X509V3_EXT_conf_nid(NULL, &ctx, nid, data.c_str())) {
                    continue;
                }

                X509_add_ext(x509, ext, -1);
                X509_EXTENSION_free(ext);
            }

            // Now set the issuer name.
            X509_set_issuer_name(x509, X509_get_subject_name(issuer.x509));

            // Actually sign the certificate with our key.
            if (!X509_sign(x509, issuer.key, EVP_sha256())) {
                free_cert();
                throw Exception("Error signing certificate.");
            }

            LOG_DEBUG("Successfully created an x509 certificate")
        }

    public:
        /**
         * Constructs a self-signed certificate authority
         */
        Certificate() : Certificate("acidicoala", *this, {
                { NID_basic_constraints,        "critical, CA:TRUE" },
                { NID_subject_key_identifier,   "hash" },
                { NID_authority_key_identifier, "keyid:always, issuer:always" },
                { NID_key_usage,                "critical, cRLSign, digitalSignature, keyCertSign" },
            }
        ) {}

        /**
         * Constructs a server certificate signed by the provided authority
         */
        Certificate(
            const String& server_host,
            const Certificate& issuer
        ) : Certificate(koalabox::globals::get_project_name(), issuer, {
                { NID_basic_constraints,        "critical, CA:FALSE" },
                { NID_subject_key_identifier,   "hash" },
                { NID_authority_key_identifier, "keyid:always, issuer:always" },
                { NID_key_usage,                "critical, nonRepudiation, digitalSignature, keyEncipherment, keyAgreement" },
                { NID_ext_key_usage,            "critical, serverAuth" },
                { NID_subject_alt_name,         "DNS:" + server_host }
            }
        ) {}

        ~Certificate() {
            LOG_DEBUG("Freeing an x509 certificate")

            free_cert();
        }

        EVP_PKEY* get_key() const {
            return key;
        }

        X509* get_x509() const {
            return x509;
        }

        void write_to_disk(const String& cert_name) {
            const auto write = [&](const String& extension, const Function<int(FILE*)>& callback) {
                const auto file_name = cert_name + "." + extension;
                const auto file_path = (koalabox::paths::get_self_path() / file_name).string();

                LOG_DEBUG("Writing {}", file_path)

                FILE* stream;
                if (fopen_s(&stream, file_path.c_str(), "wb")) {
                    throw koalabox::util::exception("Unable to open {} for writing", file_path);
                }

                auto key_success = callback(stream);
                fclose(stream);

                if (not key_success) {
                    throw koalabox::util::exception("Unable to write {} to disk", file_path);
                }
            };

            write("key", [&](FILE* stream) {
                return PEM_write_PrivateKey(stream, get_key(), NULL, NULL, 0, NULL, NULL);
            });

            write("crt", [&](FILE* stream) {
                return PEM_write_X509(stream, get_x509());
            });
        }
    };

    void start(
        const String& local_host,
        unsigned int port,
        const String& server_host,
        const Map<String, httplib::Server::Handler>& pattern_handlers
    ) noexcept {
        std::thread([=]() {
            try {
                // ca stands for Certificate Authority
                const auto project_name = koalabox::globals::get_project_name();

                Certificate ca_cert;
                ca_cert.write_to_disk(project_name + ".ca");

                Certificate server_cert(server_host, ca_cert);
                server_cert.write_to_disk(project_name + ".server");

                httplib::SSLServer server(server_cert.get_x509(), server_cert.get_key());

                for (const auto& [pattern, handler]: pattern_handlers) {
                    server.Get(pattern, handler);
                }

                if (not server.listen(local_host, port)) {
                    throw Exception("server.listen returned false");
                }
            } catch (const Exception& e) {
                koalabox::util::panic("Error starting http server: {}", e.what());
            }
        }).detach();
    }

}
