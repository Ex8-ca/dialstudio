// Command-line front end: every subcommand except the GUI runs without a display.
#pragma once

#include <QStringList>

namespace Cli {
// Returns the process exit code. `args` excludes the program name.
int run(const QStringList &args);
}
