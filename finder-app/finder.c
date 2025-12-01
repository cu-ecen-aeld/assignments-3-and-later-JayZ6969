#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <errno.h>

int main(int argc, char *argv[])
{
    openlog("writer", LOG_PID | LOG_CONS, LOG_USER);
    
    // Check if both arguments are provided
    if (argc != 3) 
    {
        syslog(LOG_ERR, "Invalid number of arguments. Expected 2, got %d!", argc - 1);
        fprintf(stderr, "Error: Two arguments required!\n");
        fprintf(stderr, "Usage: %s <writefile> <writestr>\n", argv[0]);
        closelog();
        return 1;
    }
    
    // Extract arguments
    const char *writefile = argv[1];
    const char *writestr = argv[2];

    // Log the write operation
    syslog(LOG_DEBUG, "Writing %s to %s!", writestr, writefile);

    FILE *file = fopen(writefile, "w");
    if (file == NULL) 
    {
        syslog(LOG_ERR, "Failed to open file %s for writing: %s!", writefile, strerror(errno));
        fprintf(stderr, "Error: Could not create file %s!\n", writefile);
        closelog();
        return 1;
    }

    if (fprintf(file, "%s", writestr) < 0) 
    {
        syslog(LOG_ERR, "Failed to write to file %s: %s!", writefile, strerror(errno));
        fprintf(stderr, "Error: Could not write to file %s!\n", writefile);
        fclose(file);
        closelog();
        return 1;
    }
    
    // Close the file
    if (fclose(file) != 0) 
    {
        syslog(LOG_ERR, "Failed to close file %s: %s!", writefile, strerror(errno));
        fprintf(stderr, "Error: Could not close file %s!\n", writefile);
        closelog();
        return 1;
    }
    
    // Close syslog
    closelog();
    
    return 0;
}
