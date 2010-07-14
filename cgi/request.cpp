#include "m2pp-cgi.hpp"
#include <cstdlib>
#include <cctype>
#include <climits>
#include <sstream>
#include <algorithm>
#include <iostream>
#include <libgen.h>
#include <unistd.h>
#include <signal.h>
#include <poll.h>
#include <fcntl.h>

// implementation according to RFC 3875.

static std::string lookup_header(const std::vector<m2pp::header>& hdrs, const std::string& key) {
	for (std::vector<m2pp::header>::const_iterator it=hdrs.begin();it!=hdrs.end();it++) {
		if (::strcasecmp(it->first.c_str(), key.c_str())==0) {
			return it->second;
		}
	}
	return "";
}

static int normalize(int c) {
	if ('-' == c) {
		return '_';
	}
	return c;
}

static void chomp(std::string& str) {
	std::string::size_type pos = str.find_last_not_of("\r\n");
	if (pos != std::string::npos) {
		str = str.substr(0, pos+1);
	}
}

static m2pp::header parse_header(const std::string& str) {
	m2pp::header result;

	std::string::size_type pos = str.find_first_of(":");
	if (pos != std::string::npos) {
		result.first = str.substr(0, pos);
		pos = str.find_first_not_of(": ", pos);
		if (pos != std::string::npos) {
			result.second = str.substr(pos, str.length() - pos);
		}
	} else {
		result.first = str;
	}

	return result;
}

void handle_request(m2pp::connection& conn, m2pp::request& req, const std::string& cgidir) {
	std::string fullpath = cgidir + req.path;

	std::vector<std::string> env;

#if 0
	// TODO: fix this mess.
	// make sure that full script path is within the specified CGI directory.
	char * resolved_path = ::realpath(fullpath.c_str(), NULL);
	if (!resolved_path || cgidir.length() > strlen(resolved_path) || cgidir.substr(0, strlen(resolved_path)) != resolved_path) {
		printf("resolved_path = %s cgidir = %s fullpath = %s\n", resolved_path, cgidir.c_str(), fullpath.c_str());
		conn.reply_http(req, "Forbidden", 403, "Forbidden");
		return;
	}

	fullpath = resolved_path;
#endif

	// 7.2 "The current working directory for the script SHOULD be set to the directory containing the script."
	char scriptdir[PATH_MAX+1];
	::snprintf(scriptdir, sizeof(scriptdir), "%s", fullpath.c_str());
	::dirname(scriptdir);
	::chdir(scriptdir);

	// 4.1 Request Meta-Variables
	// 7.2 "Meta-variables are passed to the script in identically named environment variables."

	// TODO: AUTH_TYPE

	// 4.1.2 CONTENT_LENGTH
	if (req.body.length() > 0) {
		std::ostringstream cl;
		cl << req.body.length();
		env.push_back(std::string("CONTENT_LENGTH=") + cl.str());

		// 4.1.3 CONTENT_TYPE
		std::string content_type = lookup_header(req.headers, "Content-Type");
		if (content_type != "") {
			env.push_back(std::string("CONTENT_TYPE=") + content_type);
		}
	}

	// 4.1.4 GATEWAY_INTERFACE
	env.push_back("GATEWAY_INTERFACE=CGI/1.1");

	// TODO: PATH_INFO

	// 4.1.6 PATH_TRANSLATED

	std::string uri = lookup_header(req.headers, "URI");
	std::string host = lookup_header(req.headers, "Host");
	if (host != "" && uri != "") {
		std::ostringstream path;
		path << "http://" << host << uri;
		env.push_back(std::string("PATH_TRANSLATED=") + path.str());
	}

	// 4.1.7 QUERY_STRING
	std::string query = lookup_header(req.headers, "QUERY");
	if (query != "") {
		env.push_back(std::string("QUERY_STRING=") + query);
	}

	// TODO: REMOTE_ADDR
	// TODO: REMOTE_HOST
	// TODO: REMOTE_IDENT
	// TODO: REMOTE_USER

	// 4.1.12 REQUEST_METHOD
	std::string method = lookup_header(req.headers, "METHOD");
	if (method != "") {
		env.push_back(std::string("REQUEST_METHOD=") + method);
	}

	// 4.1.13 SCRIPT_NAME
	env.push_back(std::string("SCRIPT_NAME=") + req.path);

	std::string::size_type pos1 = host.find_first_of(":", 0);
	std::string::size_type pos2 = host.find_first_not_of(":", pos1);
	std::string server_name = host.substr(0, pos1);
	std::string server_port = host.substr(pos2, host.length() - pos2);

	// 4.1.14 SERVER_NAME
	if (server_name != "") {
		env.push_back(std::string("SERVER_NAME=") + server_name);
	}

	// 4.1.15 SERVER_PORT
	if (server_port != "") {
		env.push_back(std::string("SERVER_PORT=") + server_port);
	}

	// 4.1.16 SERVER_PROTOCOL
	std::string server_protocol = lookup_header(req.headers, "VERSION");
	if (server_protocol != "") {
		env.push_back(std::string("SERVER_PROTOCOL=") + server_protocol);
	} else {
		env.push_back("SERVER_PROTOCOL=HTTP/1.0");
	}

	// 4.1.17 SERVER_SOFTWARE
	env.push_back("SERVER_SOFTWARE=m2pp-cgi");

	// 4.1.18 Protocol-Specific Meta-Variables

	for (std::vector<m2pp::header>::iterator it=req.headers.begin();it!=req.headers.end();it++) {
		if (it->first != "PATH" && it->first != "METHOD" && it->first != "VERSION" && it->first != "URI" && it->first != "QUERY") {
			std::string variable_name = "HTTP_" + it->first;
			std::transform(variable_name.begin(), variable_name.end(), variable_name.begin(), static_cast<int (*)(int)>(std::toupper));
			std::transform(variable_name.begin(), variable_name.end(), variable_name.begin(), normalize);
			env.push_back(variable_name + "=" + it->second);
		}
	}

	int fd_stdin[2];
	int fd_stdout[2];
	//int fd_stderr[2];
	pipe(fd_stdin);
	pipe(fd_stdout);
	//pipe(fd_stderr);
	int devnull = open("/dev/null", O_WRONLY);
	unsigned int cgienvsize = env.size();
	char ** cgienv;


	int cgipid = fork();
	switch (cgipid) {
		case 0:
			cgienv = static_cast<char **>(calloc(cgienvsize + 1, sizeof(char *)));
			for (unsigned int i=0;i<cgienvsize;i++) {
				cgienv[i] = strdup(env[i].c_str());
			}
			close(fd_stdin[1]);
			close(fd_stdout[0]);
			//close(fd_stderr[0]);
			dup2(fd_stdin[0], 0);
			dup2(fd_stdout[1], 1);
			dup2(devnull, 2);
			// TODO: set timeout using alarm(2) to limit execution time of CGI script.
			execle(fullpath.c_str(), fullpath.c_str(), NULL, cgienv);
			std::cerr << "error: exec(" << fullpath << ") failed: " << ::strerror(errno) << std::endl;
			break;
		case -1:
			std::cerr << "fork failed: " << ::strerror(errno) << std::endl;
			conn.reply_http(req, "Internal Server Error", 500, "Internal Server Error");
			return;
		default:
			close(fd_stdin[0]);
			close(fd_stdout[1]);
			//close(fd_stderr[1]);
			break;
	}

	if (req.body.length() > 0) {
		size_t size = req.body.size();
		size_t offs = 0;
		int rc;
		while (offs < size) {
			rc = ::write(fd_stdin[1], req.body.data() + offs, req.body.size() - offs);
			if (rc < 0)
				break;
			offs += rc;
		}
		if (rc < 0) {
			std::cerr << "sending body to CGI script failed: " << ::strerror(errno) << std::endl;
			conn.reply_http(req, "Internal Server Error", 500, "Internal Server Error");
			::kill(cgipid, SIGTERM);
			return;
		}
	}

	std::string coll_stdout;

#if 0
	std::string coll_stderr;
	int stdout_eof = 0;
	int stderr_eof = 0;

	struct pollfd pollfds[2] = {
		{ fd_stdout[0], POLLIN, 0 },
		{ fd_stderr[0], POLLIN, 0 }
	};
#endif

	char buf[40960];

	int size = read(fd_stdout[0], buf, sizeof(buf));
	while (size > 0) {
		coll_stdout.append(buf, size);
		size = read(fd_stdout[0], buf, sizeof(buf));
	}

#if 0

	do {

		printf("before poll\n");

		int rc = poll(pollfds, 2, -1);

		printf("after poll: rc = %d\n", rc);

		if (rc >= 0) {
			printf("stdout.revents = %x POLLIN = %x\n", pollfds[0].revents, POLLIN);
			if (pollfds[0].revents & POLLIN) {
				int size = read(fd_stdout[0], buf, sizeof(buf));
				printf("after read for stdout: size = %d\n", size);
				if (size <= 0) {
					stdout_eof = 1;
				} else {
					coll_stdout.append(buf, size);
				}
			}
			printf("stderr.revents = %x POLLIN = %x\n", pollfds[1].revents, POLLIN);
			if (pollfds[1].revents & POLLIN) {
				int size = read(fd_stdout[0], buf, sizeof(buf));
				printf("after read for stderr: size = %d\n", size);
				if (size <= 0) {
					stderr_eof = 1;
				} else {
					coll_stderr.append(buf, size);
				}
			}
		}

	} while (stdout_eof == 0 || stderr_eof == 0);

	if (coll_stderr.length() > 0) {
		std::cerr << "Error " << fullpath << " pid=" << cgipid << ": " << coll_stderr << std::endl;
	}

#endif

	std::vector<m2pp::header> resphdrs;

	std::istringstream script_output(coll_stdout);

	std::string hdr;

	std::getline(script_output, hdr);
	chomp(hdr);

	while (hdr != "") {
		resphdrs.push_back(parse_header(hdr));

		std::getline(script_output, hdr);
		chomp(hdr);
	}

	std::string final_output;

	while (!script_output.eof()) {
		script_output.read(buf, sizeof(buf));
		final_output.append(buf, script_output.gcount());
	}

	conn.reply_http(req, final_output, 200, "OK", resphdrs);
}
