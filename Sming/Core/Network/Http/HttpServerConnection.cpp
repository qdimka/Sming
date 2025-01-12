/****
 * Sming Framework Project - Open Source framework for high efficiency native ESP8266 development.
 * Created 2015 by Skurydin Alexey
 * http://github.com/SmingHub/Sming
 * All files of the Sming Core are provided under the LGPL v3 license.
 *
 * HttpServerConnection.cpp
 *
 * Modified: 2017 - Slavey Karadzhov <slav@attachix.com>
 *
 ****/

#include "HttpServerConnection.h"
#include "HttpResourceTree.h"
#include "Network/HttpServer.h"
#include "Network/TcpServer.h"
#include "Network/WebConstants.h"
#include "Data/Stream/ChunkedStream.h"
#include <SystemClock.h>

#if HTTP_SERVER_EXPOSE_VERSION == 1
#include <SmingVersion.h>
#endif

int HttpServerConnection::onMessageBegin(http_parser* parser)
{
	// Reset Response ...
	response.reset();

	// ... and Request
	request.setMethod((const HttpMethod)parser->method);

	// and temp data...
	reset();
	bodyParser = nullptr;
	hasContentError = false;

	return 0;
}

int HttpServerConnection::onPath(const Url& uri)
{
	if(resourceTree == nullptr) {
		debug_e("ERROR: HttpServerConnection: The resource tree is not set!");

		return -1;
	}

	request.setURL(uri);

	resource = resourceTree->find(request.uri.Path);
	if(resource == nullptr) {
		resource = resourceTree->getDefault();
	}

	return 0;
}

int HttpServerConnection::onMessageComplete(http_parser* parser)
{
	// we are finished with this request
	int hasError = 0;

	if(bodyParser) {
		bodyParser(request, nullptr, PARSE_DATAEND);
	}

	if(hasContentError) {
		response.code = HTTP_STATUS_BAD_REQUEST;
	}

	if(resource != nullptr && resource->onRequestComplete) {
		hasError = resource->onRequestComplete(*this, request, response);
	}

	if(request.responseStream != nullptr) {
		delete request.responseStream;
		request.responseStream = nullptr;
	}

	if(!hasError) {
		send();
	}

	return hasError;
}

int HttpServerConnection::onHeadersComplete(const HttpHeaders& headers)
{
	/* Callbacks should return non-zero to indicate an error. The parser will
	 * then halt execution.
	 *
	 * The one exception is on_headers_complete. In a HTTP_RESPONSE parser
	 * returning '1' from on_headers_complete will tell the parser that it
	 * should not expect a body. This is used when receiving a response to a
	 * HEAD request which may contain 'Content-Length' or 'Transfer-Encoding:
	 * chunked' headers that indicate the presence of a body.
	 *
	 * Returning `2` from on_headers_complete will tell parser that it should not
	 * expect neither a body nor any further responses on this connection. This is
	 * useful for handling responses to a CONNECT request which may not contain
	 * `Upgrade` or `Connection: upgrade` headers.
	 */
	int error = 0;
	request.setHeaders(headers);

	if(resource != nullptr && resource->onHeadersComplete) {
		error = resource->onHeadersComplete(*this, request, response);
	}

	if(!error && request.method == HTTP_HEAD) {
		error = 1;
	}

	if(bodyParsers != nullptr && request.headers.contains(HTTP_HEADER_CONTENT_TYPE)) {
		String contentType = request.headers[HTTP_HEADER_CONTENT_TYPE];
		int endPos = contentType.indexOf(';');
		if(endPos != -1) {
			contentType = contentType.substring(0, endPos);
		}

		// Content-Type for exact type: application/json
		int i = bodyParsers->indexOf(contentType);
		if(i < 0) {
			// Wildcard type for application: application/*
			contentType.setLength(contentType.indexOf('/') + 1);
			contentType += '*';
			i = bodyParsers->indexOf(contentType);
			if(i < 0) {
				// Wildcard type for the rest*
				i = bodyParsers->indexOf(String('*'));
			}
		}

		if(i >= 0) {
			bodyParser = bodyParsers->valueAt(i);
			assert(bodyParser != nullptr);
			bodyParser(request, nullptr, PARSE_DATASTART);
		}
	}

	return error;
}

int HttpServerConnection::onBody(const char* at, size_t length)
{
	if(hasContentError) {
		return 0;
	}

	if(bodyParser) {
		const size_t consumed = bodyParser(request, at, length);
		if(consumed != length) {
			hasContentError = true;
			if(closeOnContentError) {
				return -1;
			}
		}
	}

	if(resource != nullptr && resource->onBody) {
		const int result = resource->onBody(*this, request, at, length);
		if(result != 0) {
			hasContentError = true;
			if(closeOnContentError) {
				return result;
			}
		}
	}

	return 0;
}

bool HttpServerConnection::onHttpError(http_errno error)
{
	response.code = HTTP_STATUS_BAD_REQUEST;
	int hasError = onMessageComplete(nullptr);
	if(hasError) {
		sendError();
	}

	HttpConnection::onHttpError(error);

	return true;
}

void HttpServerConnection::onReadyToSendData(TcpConnectionEvent sourceEvent)
{
	switch(state) {
	case eHCS_StartSending: {
		sendResponseHeaders(&response);
		state = eHCS_SendingHeaders;
	}

	case eHCS_SendingHeaders: {
		if(stream != nullptr && !stream->isFinished()) {
			break;
		}

		state = eHCS_StartBody;
	}

	case eHCS_StartBody:
	case eHCS_SendingBody: {
		if(!sendResponseBody(&response)) {
			break;
		}

		delete stream;
		stream = nullptr;
		state = eHCS_Sent;
	}

	case eHCS_Sent: {
		if(response.headers[HTTP_HEADER_CONNECTION] == _F("close")) {
			setTimeOut(1); // decrease the timeout to 1 tick
		}

		response.reset();
		request.reset();

		state = eHCS_Ready;

		break;
	}

	default:; // Do nothing

	} /* switch(state) */

	TcpClient::onReadyToSendData(sourceEvent);
}

void HttpServerConnection::sendResponseHeaders(HttpResponse* response)
{
#ifndef DISABLE_HTTPSRV_ETAG
	if(response->stream != nullptr && !response->headers.contains(HTTP_HEADER_ETAG)) {
		String tag = response->stream->id();
		if(tag.length() > 0) {
			response->headers[HTTP_HEADER_ETAG] = String('"' + tag + '"');
		}
	}

	if(request.headers.contains(HTTP_HEADER_IF_MATCH) && response->headers.contains(HTTP_HEADER_ETAG) &&
	   request.headers[HTTP_HEADER_IF_MATCH] == response->headers[HTTP_HEADER_ETAG]) {
		if(request.method == HTTP_GET || request.method == HTTP_HEAD) {
			response->code = HTTP_STATUS_NOT_MODIFIED;
			response->headers[HTTP_HEADER_CONTENT_LENGTH] = "0";
			delete response->stream;
			response->stream = nullptr;
		}
	}
#endif /* DISABLE_HTTPSRV_ETAG */

	String statusLine =
		F("HTTP/1.1 ") + String(response->code) + ' ' + httpGetStatusText((enum http_status)response->code) + "\r\n";
	sendString(statusLine);
	if(response->stream != nullptr && response->stream->available() >= 0) {
		response->headers[HTTP_HEADER_CONTENT_LENGTH] = String(response->stream->available());
	}
	if(!response->headers.contains(HTTP_HEADER_CONTENT_LENGTH) && response->stream == nullptr) {
		response->headers[HTTP_HEADER_CONTENT_LENGTH] = "0";
	}

	if(!response->headers.contains(HTTP_HEADER_CONNECTION)) {
		if(request.headers[HTTP_HEADER_CONNECTION] == _F("close")) {
			// the other side requests closing of the tcp connection...
			response->headers[HTTP_HEADER_CONNECTION] = _F("close");
		} else {
			response->headers[HTTP_HEADER_CONNECTION] = _F("keep-alive"); // Keep-Alive to reuse the connection
		}
	}

#if HTTP_SERVER_EXPOSE_NAME == 1
	if(!response->headers.contains(HTTP_HEADER_SERVER)) {
		String s = _F("HttpServer/Sming");
#if HTTP_SERVER_EXPOSE_VERSION == 1
		s += _F(" Sming/" SMING_VERSION);
#endif
		response->headers[HTTP_HEADER_SERVER] = s;
	}
#endif

	if(SystemClock.isSet()) {
		response->headers[HTTP_HEADER_DATE] = DateTime(SystemClock.now(eTZ_UTC)).toHTTPDate();
	}

	for(unsigned i = 0; i < response->headers.count(); i++) {
		sendString(response->headers[i]);
	}
	sendString("\r\n");
}

bool HttpServerConnection::sendResponseBody(HttpResponse* response)
{
	if(state == eHCS_StartBody) {
		state = eHCS_SendingBody;
		if(request.method == HTTP_HEAD) {
			if(response->stream != nullptr) {
				delete response->stream;
				response->stream = nullptr;
			}
			return true;
		}

		if(response->stream == nullptr) {
			return true;
		}

		delete stream;
		if(response->headers[HTTP_HEADER_TRANSFER_ENCODING] == _F("chunked")) {
			stream = new ChunkedStream(response->stream);
		} else {
			stream = response->stream; // avoid intermediate buffers
		}
		response->stream = nullptr;

		return false;
	}

	if(stream == nullptr) {
		// we are done for now
		return true;
	}

	if(response->stream == nullptr && !stream->isFinished()) {
		return false;
	}

	return true;
}

void HttpServerConnection::sendError(const String& message, enum http_status code)
{
	debug_d("SEND ERROR PAGE");
	response.reset();
	response.code = code;
	response.setContentType(MIME_HTML);

	String html = F("<H2 color='#444'>");
	html += message ? message : httpGetStatusText((enum http_status)response.code);
	html += F("</H2>");
	response.headers[HTTP_HEADER_CONTENT_LENGTH] = html.length();
	response.headers[HTTP_HEADER_CONNECTION] = _F("close");
	response.sendString(html);

	send();
}
