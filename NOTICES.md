# Third-party notices

Tortilla is © Manu Evans and licensed under the GNU General Public License,
version 3 (see [LICENSE](LICENSE)). It's built on the work of others, whose
licences follow.

## Built into Tortilla

- **rapport**, Tortilla's native UI and system library.
  © Manu Evans. MIT licence. <https://github.com/TurkeyMan/rapport>
- **libgit2**, git's object database, read by Tortilla directly. GNU GPL
  version 2 with the linking exception. <https://libgit2.org>
  The Windows build links it statically, with what it uses:
  - **PCRE2**: BSD licence. <https://github.com/PCRE2Project/pcre2>
  - **zlib**: zlib licence. <https://zlib.net>
  - **http-parser**: MIT licence. <https://github.com/nodejs/http-parser>
- **The GitHub mark** in the welcome window: Octicons' mark-github,
  © GitHub, Inc., MIT licence. <https://github.com/primer/octicons>
  Used as a link to Tortilla's repository, as GitHub's logo guidelines allow.

## Used alongside it

Tortilla runs **git** (GNU GPL version 2), which it doesn't ship, and on Linux
uses the desktop's own GTK or Qt and, for pictures, gdk-pixbuf.

## Online

The log shows commit authors' pictures from **Gravatar**
(<https://gravatar.com>): it asks for each address's SHA-256 hash, never the
address itself, and keeps the answers for a week.
