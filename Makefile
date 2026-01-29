# Compiler and Flags
CC      = gcc
CFLAGS  = -Wall -Wextra -Werror -I./include

# Directories
SRCDIR  = src
OBJDIR  = obj
BINDIR  = bin

# Executables
SERVER_EXE = $(BINDIR)/server
CLIENT_EXE = $(BINDIR)/client

# Rules
.PHONY: all clean directories

all: directories $(SERVER_EXE) $(CLIENT_EXE)

# Create necessary directories
directories:
	@mkdir -p $(OBJDIR)
	@mkdir -p $(BINDIR)

# Link Server
$(SERVER_EXE): $(OBJDIR)/server.o
	@echo "Linking Server..."
	$(CC) $(CFLAGS) $^ -o $@

# Link Client
$(CLIENT_EXE): $(OBJDIR)/client.o
	@echo "Linking Client..."
	$(CC) $(CFLAGS) $^ -o $@

# Compile Source files to Object files
$(OBJDIR)/%.o: $(SRCDIR)/%.c
	@echo "Compiling $<..."
	$(CC) $(CFLAGS) -c $< -o $@

# Utility Rules
clean:
	@echo "Cleaning up..."
	rm -rf $(OBJDIR) $(BINDIR)