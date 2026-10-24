// Answering git's and ssh's questions in a window: tortilla is their askpass
// program (GIT_ASKPASS, SSH_ASKPASS), as TortoiseGit's SshAskPass is. The
// git tortilla runs is pointed at tortilla itself with TORTILLA_ASKPASS set;
// started so, with the question as its one argument, tortilla asks it and
// writes the answer to its output.
//   a password or passphrase   a hidden entry
//   a username                 a plain entry
//   a yes/no question          Yes and No (ssh's unknown host key)
#pragma once

#include <rapport/rapport.h>

#include <string>

namespace tortilla
{

// Whether this process was started to answer a question (the one argument).
bool started_as_askpass(int argc);

// Asks the question; 0 with the answer written, 1 if cancelled.
int run_askpass(rap::ui&, const std::string& question);

}  // namespace tortilla
