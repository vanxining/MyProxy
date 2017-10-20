/***********************************************************************
 main.cpp - The main() routine for all the "Basic Winsock" suite of
    programs from the Winsock Programmer's FAQ.  This function parses
    the command line, starts up Winsock, and calls an external function
    called DoWinsock to do the actual work.

 This program is hereby released into the public domain.  There is
 ABSOLUTELY NO WARRANTY WHATSOEVER for this product.  Caveat hacker.
***********************************************************************/

#include <winsock2.h>
#pragma comment(lib, "ws2_32.lib")

#include <stdlib.h>
#include <iostream>

using namespace std;


//// Prototypes ////////////////////////////////////////////////////////

extern int DoWinsock(const char *pcHost, const char *pcPort);


//// Constants /////////////////////////////////////////////////////////

// Default port to connect to on the server
const char *kDefaultServerPort = "1990";


//// main //////////////////////////////////////////////////////////////

int main(int argc, char *argv[]) {
	const char *pcPort = kDefaultServerPort;

    // Do we have enough command line arguments?
    if (argc >= 2) {
        pcPort = argv[1];
    }

    // Do a little sanity checking because we're anal.
    int nNumArgsIgnored = (argc - 2);
    if (nNumArgsIgnored > 0) {
        cerr << nNumArgsIgnored << " extra argument" <<
               (nNumArgsIgnored == 1 ? "" : "s") << " ignored.  FYI." << endl;
    }

    // Start Winsock up.
    WSAData wsaData;
	int nCode;
    if ((nCode = WSAStartup(MAKEWORD(2, 2), &wsaData)) != 0) {
		cerr << "WSAStartup() returned error code " << nCode << "." << endl;
        return 255;
    }

    // Call the main example routine.
    int retval = DoWinsock("127.0.0.1", pcPort);

    // Shut Winsock back down and take off.
    WSACleanup();
    return retval;
}
