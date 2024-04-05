#include <stdio.h>
#include <unistd.h>
#include <sys/types.h>
#include <stdlib.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>
#include <sys/ioctl.h>
#include "ioctl_commands.h"

int main(int argc, char const *argv[]) {
    //Check if a message was provided as an argument
    if(argc <= 1) {
        //If it was not, print usage instructions and exit
        fprintf(stderr, "Usage: %s <message>\n", argv[0]);
        return 1;
    }
    //Open the first device file for read-write
    int dev1 = open("/dev/dm510-0", O_RDWR);
    if(dev1 < 0) {
        perror("Error opening /dev/dm510-0");
        //If it fails, return 1
        return 1;
    }
    //Open secound device file for read-write
    int dev2 = open("/dev/dm510-1", O_RDWR);
    if(dev2 < 0) {
        perror("Error opening /dev/dm510-1");
        close(dev1); // Close the first device if the second fails to open
        //Return 1 if it fails
        return 1;
    }

    //Store message from command line arguments
    const char *msg = argv[1];
    //Calculate the size of the message
    size_t size = strlen(msg);
    printf("Written message: '%s'\n", msg);


    ssize_t bytesWritten = write(dev1, msg, size);
    if(bytesWritten < 0) {
        perror("Failed to write to device");
        close(dev1);
        close(dev2);
        return 1;
    }

    //Allocate memory for reading the message back, +1 for null terminator
    char *buf = malloc(size + 1); // Allocate buffer for the read message plus null terminator
    if(buf == NULL) {
        //If it fails, print error message and clean up 
        perror("Failed to allocate buffer");
        close(dev1);
        close(dev2);
        return 1;
    }

    //Attempt to read the message from the second device
    ssize_t bytesRead = read(dev2, buf, size);
    if(bytesRead < 0) {
        //If it fails, print error message and clean up 
        perror("Error reading from device");
        free(buf);
        close(dev1);
        close(dev2);
        return 1;
    } else {
        buf[bytesRead] = '\0';
        printf("Message read : '%s'\n", buf);
    }

    //clean up
    free(buf);
    close(dev1);
    close(dev2);

    return 0;
};
