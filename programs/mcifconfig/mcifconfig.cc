/*
 *    Copyright 2021-Present Couchbase, Inc.
 *
 *   Use of this software is governed by the Business Source License included
 *   in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
 *   in that file, in accordance with the Business Source License, use of this
 *   software will be governed by the Apache License, Version 2.0, included in
 *   the file licenses/APL2.txt.
 */

#include <boost/filesystem.hpp>
#include <folly/portability/Unistd.h>
#include <getopt.h>
#include <platform/dirutils.h>
#include <programs/getpass.h>
#include <programs/hostname_utils.h>
#include <protocol/connection/client_connection.h>
#include <protocol/connection/client_mcbp_commands.h>
#include <protocol/connection/frameinfo.h>
#include <utilities/string_utilities.h>
#include <utilities/terminal_color.h>
#include <utilities/terminate_handler.h>
#include <iostream>

static void usage() {
    std::cerr << R"(Usage mcifconfig [options] <tls|list|define|delete>

Options:

  --host hostname[:port]   The host (with an optional port) to connect to
  --port port              The port number to connect to
  --user username          The name of the user to authenticate as
  --password password      The passord to use for authentication
                           (use '-' to read from standard input, or
                           set the environment variable CB_PASSWORD)
  --tls[=cert,key]         Use TLS and optionally try to authenticate
                           by using the provided certificate and
                           private key.
  --ipv4                   Connect over IPv4
  --ipv6                   Connect over IPv6
  --help                   This help text

Commands:

   list                    List the defined interfaces
   define <filename/JSON>  Define a new interface
   delete <UUID>           Delete the interface with the provided UUID
   tls [filename/JSON]     Get (no argument) or set TLS properties

)";

    exit(EXIT_FAILURE);
}

/**
 * Get the payload to use. If param is a filename then read the file, if not
 * it should be the actual value
 *
 * @param param the parameter passed to the program
 */
std::string getPayload(const std::string& param) {
    boost::filesystem::path nm(param);
    std::string value = param;
    if (exists(nm)) {
        value = cb::io::loadFile(nm.generic_string());
    }
    try {
        const auto parsed = nlohmann::json::parse(value);
    } catch (const std::exception& e) {
        std::cerr << TerminalColor::Red
                  << "Failed to parse provided JSON: " << e.what()
                  << TerminalColor::Reset << std::endl;
        std::exit(EXIT_FAILURE);
    }
    return value;
}

int main(int argc, char** argv) {
    // Make sure that we dump callstacks on the console
    install_backtrace_terminate_handler();
#ifndef WIN32
    setTerminalColorSupport(isatty(STDOUT_FILENO) && isatty(STDERR_FILENO));
#endif

    int cmd;
    std::string port;
    std::string host{"localhost"};
    std::string user{};
    std::string password{};
    std::string ssl_cert;
    std::string ssl_key;
    sa_family_t family = AF_UNSPEC;
    bool secure = false;

    cb::net::initialize();

    // we could have used an array, but then we need to keep track of the
    // size. easier to just use a vector
    const std::vector<option> options{
            {"ipv4", no_argument, nullptr, '4'},
            {"ipv6", no_argument, nullptr, '6'},
            {"host", required_argument, nullptr, 'h'},
            {"port", required_argument, nullptr, 'p'},
            {"password", required_argument, nullptr, 'P'},
            {"user", required_argument, nullptr, 'u'},
            {"tls=", optional_argument, nullptr, 't'},
            {"help", no_argument, nullptr, 0},
            {nullptr, 0, nullptr, 0}};

    while ((cmd = getopt_long(argc, argv, "", options.data(), nullptr)) !=
           EOF) {
        switch (cmd) {
        case '6':
            family = AF_INET6;
            break;
        case '4':
            family = AF_INET;
            break;
        case 'h':
            host.assign(optarg);
            break;
        case 'p':
            port.assign(optarg);
            break;
        case 'u':
            user.assign(optarg);
            break;
        case 'P':
            password.assign(optarg);
            break;
        case 't':
            secure = true;
            if (optarg) {
                auto parts = split_string(optarg, ",");
                if (parts.size() != 2) {
                    std::cerr << TerminalColor::Red
                              << "Incorrect format for --tls=certificate,key"
                              << TerminalColor::Reset << std::endl;
                    exit(EXIT_FAILURE);
                }
                ssl_cert = std::move(parts.front());
                ssl_key = std::move(parts.back());

                if (!cb::io::isFile(ssl_cert)) {
                    std::cerr << TerminalColor::Red << "Certificate file "
                              << ssl_cert << " does not exists"
                              << TerminalColor::Reset << std::endl;
                    exit(EXIT_FAILURE);
                }

                if (!cb::io::isFile(ssl_key)) {
                    std::cerr << TerminalColor::Red << "Private key file "
                              << ssl_key << " does not exists"
                              << TerminalColor::Reset << std::endl;
                    exit(EXIT_FAILURE);
                }
            }
            break;
        default:
            usage();
        }
    }

    if (password == "-") {
        password.assign(getpass());
    } else if (password.empty()) {
        const char* env_password = std::getenv("CB_PASSWORD");
        if (env_password) {
            password = env_password;
        }
    }

    if (optind == argc) {
        usage();
    }

    std::string key;
    std::string value;
    const std::string command{argv[optind++]};
    if (command == "list") {
        key = command;
        if (optind < argc) {
            std::cerr << TerminalColor::Red
                      << "Error: list don't take parameters"
                      << TerminalColor::Reset << std::endl;
            std::exit(EXIT_FAILURE);
        }
    } else if (command == "tls") {
        key = command;
        if (optind + 1 < argc) {
            std::cerr << TerminalColor::Red
                      << "Error: tls have 1 (optional) parameter"
                      << TerminalColor::Reset << std::endl;
            std::exit(EXIT_FAILURE);
        }
        if (optind < argc) {
            value = getPayload(argv[optind]);
        }
    } else if (command == "define") {
        key = command;
        if (optind == argc || optind + 1 < argc) {
            std::cerr << TerminalColor::Red << "Error: define have 1 parameter"
                      << TerminalColor::Reset << std::endl;
            std::exit(EXIT_FAILURE);
        }
        value = getPayload(argv[optind]);
    } else if (command == "delete") {
        key = command;
        if (optind == argc || optind + 1 < argc) {
            std::cerr << TerminalColor::Red
                      << "Error: delete must have 1 parameter"
                      << TerminalColor::Reset << std::endl;
            std::exit(EXIT_FAILURE);
        }
        value = argv[optind];
    } else {
        std::cerr << TerminalColor::Red << "Error: Unknown command \""
                  << command << "\"" << TerminalColor::Reset << std::endl;
        std::exit(EXIT_FAILURE);
    }

    try {
        if (port.empty()) {
            port = secure ? "11207" : "11210";
        }
        in_port_t in_port;
        sa_family_t fam;
        std::tie(host, in_port, fam) = cb::inet::parse_hostname(host, port);

        if (family == AF_UNSPEC) { // The user may have used -4 or -6
            family = fam;
        }

        MemcachedConnection connection(host, in_port, family, secure);
        connection.setSslCertFile(ssl_cert);
        connection.setSslKeyFile(ssl_key);
        connection.connect();

        if (!user.empty()) {
            connection.authenticate(
                    user, password, connection.getSaslMechanisms());
        }

        connection.setAgentName("mcifconfig " MEMCACHED_VERSION);
        connection.setFeatures({cb::mcbp::Feature::XERROR});

        auto rsp = connection.execute(BinprotGenericCommand{
                cb::mcbp::ClientOpcode::Ifconfig, key, value});
        if (rsp.isSuccess()) {
            std::cout << TerminalColor::Green << rsp.getDataString()
                      << TerminalColor::Reset << std::endl;
        } else {
            std::cerr << TerminalColor::Red
                      << "Failed: " << to_string(rsp.getStatus())
                      << rsp.getDataString() << TerminalColor::Reset
                      << std::endl;
            std::exit(EXIT_FAILURE);
        }
    } catch (const ConnectionError& ex) {
        std::cerr << TerminalColor::Red << ex.what() << TerminalColor::Reset
                  << std::endl;
        return EXIT_FAILURE;
    } catch (const std::runtime_error& ex) {
        std::cerr << TerminalColor::Red << ex.what() << TerminalColor::Reset
                  << std::endl;
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}
