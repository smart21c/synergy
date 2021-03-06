/*
 * synergy -- mouse and keyboard sharing utility
 * Copyright (C) 2015 Synergy Si Ltd.
 * 
 * This package is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * found in the file LICENSE that should have accompanied this file.
 * 
 * This package is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "SecureSocket.h"

#include "net/TSocketMultiplexerMethodJob.h"
#include "net/TCPSocket.h"
#include "mt/Lock.h"
#include "arch/XArch.h"
#include "base/Log.h"

#include <openssl/ssl.h>
#include <openssl/err.h>
#include <cstring>
#include <cstdlib>
#include <memory>
#include <fstream>

//
// SecureSocket
//

#define MAX_ERROR_SIZE 65535

enum {
	// this limit seems extremely high, but mac client seem to generate around
	// 50,000 errors before they establish a connection (wtf?)
	kMaxRetryCount = 100000
};

static const char kFingerprintDirName[] = "SSL/Fingerprints";
//static const char kFingerprintLocalFilename[] = "Local.txt";
static const char kFingerprintTrustedServersFilename[] = "TrustedServers.txt";
//static const char kFingerprintTrustedClientsFilename[] = "TrustedClients.txt";

struct Ssl {
	SSL_CTX*	m_context;
	SSL*		m_ssl;
};

SecureSocket::SecureSocket(
		IEventQueue* events,
		SocketMultiplexer* socketMultiplexer) :
	TCPSocket(events, socketMultiplexer),
	m_secureReady(false),
	m_fatal(false),
	m_maxRetry(kMaxRetryCount)
{
}

SecureSocket::SecureSocket(
		IEventQueue* events,
		SocketMultiplexer* socketMultiplexer,
		ArchSocket socket) :
	TCPSocket(events, socketMultiplexer, socket),
	m_secureReady(false),
	m_fatal(false),
	m_maxRetry(kMaxRetryCount)
{
}

SecureSocket::~SecureSocket()
{
	isFatal(true);
	if (m_ssl->m_ssl != NULL) {
		SSL_shutdown(m_ssl->m_ssl);

		SSL_free(m_ssl->m_ssl);
		m_ssl->m_ssl = NULL;
	}
	if (m_ssl->m_context != NULL) {
		SSL_CTX_free(m_ssl->m_context);
		m_ssl->m_context = NULL;
	}
	ARCH->sleep(1);
	delete m_ssl;
}

void
SecureSocket::close()
{
	isFatal(true);

	SSL_shutdown(m_ssl->m_ssl);

	TCPSocket::close();
}

void
SecureSocket::secureConnect()
{
	setJob(new TSocketMultiplexerMethodJob<SecureSocket>(
			this, &SecureSocket::serviceConnect,
			getSocket(), isReadable(), isWritable()));
}

void
SecureSocket::secureAccept()
{
	setJob(new TSocketMultiplexerMethodJob<SecureSocket>(
			this, &SecureSocket::serviceAccept,
			getSocket(), isReadable(), isWritable()));
}

int
SecureSocket::secureRead(void* buffer, int size, int& read)
{
	if (m_ssl->m_ssl != NULL) {
		LOG((CLOG_DEBUG2 "reading secure socket"));
		read = SSL_read(m_ssl->m_ssl, buffer, size);
		
		static int retry;

		// Check result will cleanup the connection in the case of a fatal
		checkResult(read, retry);
		
		if (retry) {
			return 0;
		}

		if (isFatal()) {
			return -1;
		}
	}
	// According to SSL spec, the number of bytes read must not be negative and
	// not have an error code from SSL_get_error(). If this happens, it is
	// itself an error. Let the parent handle the case
	return read;
}

int
SecureSocket::secureWrite(const void* buffer, int size, int& wrote)
{
	if (m_ssl->m_ssl != NULL) {
		LOG((CLOG_DEBUG2 "writing secure socket:%p", this));

		wrote = SSL_write(m_ssl->m_ssl, buffer, size);
		
		static int retry;

		// Check result will cleanup the connection in the case of a fatal
		checkResult(wrote, retry);

		if (retry) {
			return 0;
		}

		if (isFatal()) {
			return -1;
		}
	}
	// According to SSL spec, r must not be negative and not have an error code
	// from SSL_get_error(). If this happens, it is itself an error. Let the
	// parent handle the case
	return wrote;
}

bool
SecureSocket::isSecureReady()
{
	return m_secureReady;
}

void
SecureSocket::initSsl(bool server)
{
	m_ssl = new Ssl();
	m_ssl->m_context = NULL;
	m_ssl->m_ssl = NULL;

	initContext(server);
}

bool
SecureSocket::loadCertificates(String& filename)
{
	if (filename.empty()) {
		showError("ssl certificate is not specified");
		return false;
	}
	else {
		std::ifstream file(filename.c_str());
		bool exist = file.good();
		file.close();

		if (!exist) {
			String errorMsg("ssl certificate doesn't exist: ");
			errorMsg.append(filename);
			showError(errorMsg.c_str());
			return false;
		}
	}

	int r = 0;
	r = SSL_CTX_use_certificate_file(m_ssl->m_context, filename.c_str(), SSL_FILETYPE_PEM);
	if (r <= 0) {
		showError("could not use ssl certificate");
		return false;
	}

	r = SSL_CTX_use_PrivateKey_file(m_ssl->m_context, filename.c_str(), SSL_FILETYPE_PEM);
	if (r <= 0) {
		showError("could not use ssl private key");
		return false;
	}

	r = SSL_CTX_check_private_key(m_ssl->m_context);
	if (!r) {
		showError("could not verify ssl private key");
		return false;
	}

	return true;
}

void
SecureSocket::initContext(bool server)
{
	SSL_library_init();

	const SSL_METHOD* method;
 
	// load & register all cryptos, etc.
	OpenSSL_add_all_algorithms();

	// load all error messages
	SSL_load_error_strings();

	// SSLv23_method uses TLSv1, with the ability to fall back to SSLv3
	if (server) {
		method = SSLv23_server_method();
	}
	else {
		method = SSLv23_client_method();
	}
	
	// create new context from method
	SSL_METHOD* m = const_cast<SSL_METHOD*>(method);
	m_ssl->m_context = SSL_CTX_new(m);

	// drop SSLv3 support
	SSL_CTX_set_options(m_ssl->m_context, SSL_OP_NO_SSLv3);

	if (m_ssl->m_context == NULL) {
		showError();
	}
}

void
SecureSocket::createSSL()
{
	// I assume just one instance is needed
	// get new SSL state with context
	if (m_ssl->m_ssl == NULL) {
		m_ssl->m_ssl = SSL_new(m_ssl->m_context);
	}
}

int
SecureSocket::secureAccept(int socket)
{
	createSSL();

	// set connection socket to SSL state
	SSL_set_fd(m_ssl->m_ssl, socket);
	
	LOG((CLOG_DEBUG2 "accepting secure socket"));
	int r = SSL_accept(m_ssl->m_ssl);
	
	static int retry;

	checkResult(r, retry);

	if (isFatal()) {
		// tell user and sleep so the socket isn't hammered.
		LOG((CLOG_ERR "failed to accept secure socket"));
		LOG((CLOG_INFO "client connection may not be secure"));
		m_secureReady = false;
		ARCH->sleep(1);
		return -1; // Failed, error out
	}

	// If not fatal and no retry, state is good
	if (retry == 0) {
		m_secureReady = true;
		LOG((CLOG_INFO "accepted secure socket"));
		const SSL_CIPHER* cipher = SSL_get_current_cipher(m_ssl->m_ssl);
		if(cipher != NULL) {
			char * cipherVersion = SSL_CIPHER_description(cipher, NULL, 0);
			if(cipherVersion != NULL) {
				LOG((CLOG_INFO "%s", cipherVersion));
				OPENSSL_free(cipherVersion);
			}
		}
		return 1;
	}

	// If not fatal and retry is set, not ready, and return retry
	if (retry > 0) {
		LOG((CLOG_DEBUG2 "retry accepting secure socket"));
		m_secureReady = false;
		return 0;
	}

	// no good state exists here
	LOG((CLOG_ERR "unexpected state attempting to accept connection"));
	return -1;
}

int
SecureSocket::secureConnect(int socket)
{
	createSSL();

	// attach the socket descriptor
	SSL_set_fd(m_ssl->m_ssl, socket);
	
	LOG((CLOG_DEBUG2 "connecting secure socket"));
	int r = SSL_connect(m_ssl->m_ssl);
	
	static int retry;

	checkResult(r, retry);

	if (isFatal()) {
		LOG((CLOG_ERR "failed to connect secure socket"));
		return -1;
	}

	// If we should retry, not ready and return 0
	if (retry > 0) {
		LOG((CLOG_DEBUG2 "retry connect secure socket"));
		m_secureReady = false;
		return 0;
	}

	// No error, set ready, process and return ok
	m_secureReady = true;
	if (verifyCertFingerprint()) {
		LOG((CLOG_INFO "connected to secure socket"));
		if (!showCertificate()) {
			disconnect();
			return -1;// Cert fail, error
		}
	}
	else {
		LOG((CLOG_ERR "failed to verify server certificate fingerprint"));
		disconnect();
		return -1; // Fingerprint failed, error
	}
	LOG((CLOG_DEBUG2 "connected secure socket"));
	const SSL_CIPHER* cipher = SSL_get_current_cipher(m_ssl->m_ssl);
	if(cipher != NULL) {
		char * cipherVersion = SSL_CIPHER_description(cipher, NULL, 0);
		if(cipherVersion != NULL) {
			LOG((CLOG_INFO "%s", cipherVersion));
			OPENSSL_free(cipherVersion);
		}
	}
	return 1;
}

bool
SecureSocket::showCertificate()
{
	X509* cert;
	char* line;
 
	// get the server's certificate
	cert = SSL_get_peer_certificate(m_ssl->m_ssl);
	if (cert != NULL) {
		line = X509_NAME_oneline(X509_get_subject_name(cert), 0, 0);
		LOG((CLOG_INFO "server ssl certificate info: %s", line));
		OPENSSL_free(line);
		X509_free(cert);
	}
	else {
		showError("server has no ssl certificate");
		return false;
	}

	return true;
}

void
SecureSocket::checkResult(int status, int& retry)
{
	// ssl errors are a little quirky. the "want" errors are normal and
	// should result in a retry.

	int errorCode = SSL_get_error(m_ssl->m_ssl, status);

	switch (errorCode) {
	case SSL_ERROR_NONE:
		retry = 0;
		// operation completed
		break;

	case SSL_ERROR_ZERO_RETURN:
		// connection closed
		isFatal(true);
		LOG((CLOG_DEBUG "ssl connection closed"));
		break;

	case SSL_ERROR_WANT_READ:
	case SSL_ERROR_WANT_WRITE:
	case SSL_ERROR_WANT_CONNECT:
	case SSL_ERROR_WANT_ACCEPT:
		// it seems like these sort of errors are part of openssl's normal behavior,
		// so we should expect a very high amount of these. sleeping doesn't seem to
		// help... maybe you just have to swallow the errors (yuck).
		retry++;
		LOG((CLOG_DEBUG2 "passive ssl error, error=%d, attempt=%d", errorCode, retry));
		break;

	case SSL_ERROR_SYSCALL:
		LOG((CLOG_ERR "ssl error occurred (system call failure)"));
		if (ERR_peek_error() == 0) {
			if (status == 0) {
				LOG((CLOG_ERR "eof violates ssl protocol"));
			}
			else if (status == -1) {
				// underlying socket I/O reproted an error
				try {
					ARCH->throwErrorOnSocket(getSocket());
				}
				catch (XArchNetwork& e) {
					LOG((CLOG_ERR "%s", e.what()));
				}
			}
		}

		isFatal(true);
		break;

	case SSL_ERROR_SSL:
		LOG((CLOG_ERR "ssl error occurred (generic failure)"));
		isFatal(true);
		break;

	default:
		LOG((CLOG_ERR "ssl error occurred (unknown failure)"));
		isFatal(true);
		break;
	}

	// If the retry max would exceed the allowed, treat it as a fatal error
	if (retry > maxRetry()) {
		LOG((CLOG_ERR "passive ssl error limit exceeded: %d", retry));
		isFatal(true);
	}

	if (isFatal()) {
		retry = 0;
		showError();
		disconnect();
	}
}

void
SecureSocket::showError(const char* reason)
{
	if (reason != NULL) {
		LOG((CLOG_ERR "%s", reason));
	}

	String error = getError();
	if (!error.empty()) {
		LOG((CLOG_ERR "%s", error.c_str()));
	}
}

String
SecureSocket::getError()
{
	unsigned long e = ERR_get_error();

	if (e != 0) {
		char error[MAX_ERROR_SIZE];
		ERR_error_string_n(e, error, MAX_ERROR_SIZE);
		return error;
	}
	else {
		return "";
	}
}

void
SecureSocket::disconnect()
{
	sendEvent(getEvents()->forISocket().stopRetry());
	sendEvent(getEvents()->forISocket().disconnected());
	sendEvent(getEvents()->forIStream().inputShutdown());
}

void
SecureSocket::formatFingerprint(String& fingerprint, bool hex, bool separator)
{
	if (hex) {
		// to hexidecimal
		synergy::string::toHex(fingerprint, 2);
	}

	// all uppercase
	synergy::string::uppercase(fingerprint);

	if (separator) {
		// add colon to separate each 2 charactors
		size_t separators = fingerprint.size() / 2;
		for (size_t i = 1; i < separators; i++) {
			fingerprint.insert(i * 3 - 1, ":");
		}
	}
}

bool
SecureSocket::verifyCertFingerprint()
{
	// calculate received certificate fingerprint
	X509 *cert = cert = SSL_get_peer_certificate(m_ssl->m_ssl);
	EVP_MD* tempDigest;
	unsigned char tempFingerprint[EVP_MAX_MD_SIZE];
	unsigned int tempFingerprintLen;
	tempDigest = (EVP_MD*)EVP_sha1();
	int digestResult = X509_digest(cert, tempDigest, tempFingerprint, &tempFingerprintLen);

	if (digestResult <= 0) {
		LOG((CLOG_ERR "failed to calculate fingerprint, digest result: %d", digestResult));
		return false;
	}

	// format fingerprint into hexdecimal format with colon separator
	String fingerprint(reinterpret_cast<char*>(tempFingerprint), tempFingerprintLen);
	formatFingerprint(fingerprint);
	LOG((CLOG_INFO "server fingerprint: %s", fingerprint.c_str()));

	String trustedServersFilename;
	trustedServersFilename = synergy::string::sprintf(
		"%s/%s/%s",
		ARCH->getProfileDirectory().c_str(),
		kFingerprintDirName,
		kFingerprintTrustedServersFilename);

	// check if this fingerprint exist
	String fileLine;
	std::ifstream file;
	file.open(trustedServersFilename.c_str());

	bool isValid = false;
	while (!file.eof() && file.is_open()) {
		getline(file,fileLine);
		if (!fileLine.empty()) {
			if (fileLine.compare(fingerprint) == 0) {
				isValid = true;
				break;
			}
		}
	}

	file.close();
	return isValid;
}

ISocketMultiplexerJob*
SecureSocket::serviceConnect(ISocketMultiplexerJob* job,
				bool, bool write, bool error)
{
	Lock lock(&getMutex());

	int status = 0;
#ifdef SYSAPI_WIN32
	status = secureConnect(static_cast<int>(getSocket()->m_socket));
#elif SYSAPI_UNIX
	status = secureConnect(getSocket()->m_fd);
#endif

	if (status > 0) {
		return newJob();
	}
	else if (status == 0) {
		return job;
	}
	// If status < 0, error happened
	return NULL;
}

ISocketMultiplexerJob*
SecureSocket::serviceAccept(ISocketMultiplexerJob* job,
				bool, bool write, bool error)
{
	Lock lock(&getMutex());

	int status = 0;
#ifdef SYSAPI_WIN32
	status = secureAccept(static_cast<int>(getSocket()->m_socket));
#elif SYSAPI_UNIX
	status = secureAccept(getSocket()->m_fd);
#endif

	if (status > 0) {
		return newJob();
	}
	else if (status == 0) {
		return job;
	}
	// If status < 0, error happened
	return NULL;
}
