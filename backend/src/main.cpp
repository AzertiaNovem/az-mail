// Placeholder entry point (WP0). WP-A replaces this with the CLI dispatcher
// (serve|migrate|create-user|reset-password|add-domain|backup|reindex|doctor|blobs-migrate).
#include <cstdio>
#include <cstring>

int main(int argc, char** argv) {
  if (argc > 1 && (std::strcmp(argv[1], "--version") == 0 || std::strcmp(argv[1], "version") == 0)) {
    std::printf("azmail %s\n", AZMAIL_VERSION);
    return 0;
  }
  std::printf("azmail %s (placeholder build; commands are provided by WP-A)\n", AZMAIL_VERSION);
  return 0;
}
