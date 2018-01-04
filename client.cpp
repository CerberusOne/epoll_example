#include "Library/client.h"

#define BUFLEN 1024

using namespace std;

int main(int argc, char **argv) {
	int bytesSent, bytesRead;
	Client *client = new Client("127.0.0.1", 7000);
	char buffer[BUFLEN] = "hello";
	char readBuf[BUFLEN];

	if((bytesSent = write(client->GetSocket(), buffer, sizeof(buffer))) != -1) {
		printf("BytesSent: %d\n", bytesSent);
	} else {
		fprintf(stderr, "Failed to send\n");
		return -1;
	}

	if((bytesRead = read(client->GetSocket(), readBuf, sizeof(readBuf))) != -1) {
		printf("BytesRead: %d\n", bytesRead);
		printf("received: %s\n", readBuf);
	} else {
		fprintf(stderr, "Failed to read\n");
		return -2;
	}

	return 0;
}

