#!/bin/sh
# Builds a small, believable repository for screenshots: "quill", a static
# site generator, with overlapping feature branches merged back, release
# tags, remote branches, a stash and uncommitted changes.
#   tools/make_demo_repo.sh /tmp/quill
set -e
dir=${1:?usage: make_demo_repo.sh <new directory>}
[ -e "$dir" ] && { echo "$dir already exists" >&2; exit 1; }
mkdir -p "$dir"
cd "$dir"
remote="$PWD.origin.git"
git init -q -b main .
git init -q --bare "$remote"
git remote add origin "$remote"
git config user.name "Quill Bot"
git config user.email "bot@quill.example"

# commit <who> <day> <time> <message>: commits whatever is staged
commit()
{
    case $1 in
    mara) n="Mara Okafor" e="mara@quill.example" ;;
    jonas) n="Jonas Lindqvist" e="jonas@quill.example" ;;
    priya) n="Priya Raman" e="priya@quill.example" ;;
    tomas) n="Tomás Ferreira" e="tomas@quill.example" ;;
    esac
    d=$(printf '2026-08-%02dT%s+10:00' "$2" "$3")
    GIT_AUTHOR_NAME="$n" GIT_AUTHOR_EMAIL="$e" GIT_AUTHOR_DATE="$d" \
    GIT_COMMITTER_NAME="$n" GIT_COMMITTER_EMAIL="$e" GIT_COMMITTER_DATE="$d" \
        git commit -q -m "$4"
}
merge()  # merge <who> <day> <time> <branch> <message>
{
    git merge -q --no-ff --no-commit "$4" >/dev/null 2>&1
    commit "$1" "$2" "$3" "$5"
}
put() { mkdir -p "$(dirname "$1")"; printf '%s\n' "$2" >> "$1"; git add "$1"; }

put README.md "# quill
A small static site generator: Markdown in, a fast website out."
put pyproject.toml '[project]
name = "quill"
version = "0.1.0"'
commit mara 3 09:12 "Start quill: a static site generator"

put quill/__init__.py '"""quill: Markdown in, a fast website out."""'
put quill/render.py 'def render(page):
    return page.body'
commit mara 3 11:40 "Render pages from Markdown"

put templates/base.html '<!doctype html><title>{{ title }}</title>{{ body }}'
commit jonas 3 15:05 "Add the base page template"

put quill/cli.py 'def main():
    build(".")'
commit mara 4 10:22 "Add the quill build command"

put tests/test_render.py 'def test_heading():
    assert render("# Hi")'
commit priya 4 13:47 "Test heading rendering"

on() { git checkout -q "$1"; }
on_new() { git checkout -q -b "$1"; }

# Branches merge in the reverse of the order they forked, so their lanes nest
# and never cross; the work in progress below crosses twice, for interest.
on_new feature/tables
put quill/tables.py 'def table(rows):
    return "<table>"'
commit priya 5 09:30 "Render Markdown tables"
on main
put styles/site.css 'body { font-family: system-ui; max-width: 42rem; margin: auto; }'
commit jonas 5 11:15 "Add a readable default stylesheet"
on_new feature/live-reload
put quill/serve.py 'def serve(port=8000):
    watch(".")'
commit tomas 5 14:02 "Serve the site locally while editing"
on feature/tables
put quill/tables.py '    # column alignment from the separator row'
commit priya 6 10:40 "Align table columns as the separator row says"
on feature/live-reload
put quill/serve.py '    reload_browser()'
commit tomas 6 15:20 "Reload the browser when a page changes"
put quill/serve.py '    # debounce bursts of saves'
commit tomas 6 16:45 "Debounce rebuilds when saving in bursts"
on main
merge mara 7 09:05 feature/live-reload "Merge branch 'feature/live-reload'"
merge mara 7 14:30 feature/tables "Merge branch 'feature/tables'"
put docs/tables.md '# Tables
Pipes and dashes, as on GitHub.'
commit priya 7 16:00 "Document table syntax"
GIT_COMMITTER_DATE="2026-08-07T16:30+10:00" git tag -a v0.1.0 -m "quill 0.1.0" >/dev/null

on_new fix/unicode-slugs
put quill/slug.py 'def slug(title):
    return normalize(title).lower()'
commit jonas 8 10:10 "Keep accented letters in page slugs"
on main
put quill/render.py '    # syntax highlighting for fenced code'
commit priya 8 13:00 "Highlight fenced code blocks"
on fix/unicode-slugs
put tests/test_slug.py 'def test_accents():
    assert slug("Café") == "café"'
commit jonas 8 15:55 "Test slugs with accented titles"
on main
merge mara 9 11:20 fix/unicode-slugs "Merge branch 'fix/unicode-slugs'"
put pyproject.toml 'version = "0.2.0"'
commit mara 9 14:00 "Release 0.2.0"
GIT_COMMITTER_DATE="2026-08-09T14:05+10:00" git tag -a v0.2.0 -m "quill 0.2.0" >/dev/null
git push -q origin main fix/unicode-slugs 2>/dev/null

# Three features at once: search forks first and lands on main; themes and
# RSS are still in progress.
on_new feature/search
put quill/search.py 'def index(pages):
    return {p.url: p.headings for p in pages}'
commit priya 10 08:50 "Index page titles and headings for search"
on main
put pyproject.toml 'dependencies = ["markdown>=3.6"]'
commit mara 10 09:20 "Bump the Markdown parser to 3.6"
on_new feature/rss
put quill/feed.py 'def feed(pages):
    return "<rss/>"'
commit tomas 10 10:10 "Generate an RSS feed of recent posts"
on main
put README.md 'See docs/ for the full guide.'
commit mara 10 11:30 "Point the README at the docs"
git push -q origin main 2>/dev/null
on_new feature/themes
put quill/themes.py 'def load_theme(name):
    return read(f"themes/{name}")'
commit jonas 10 14:20 "Load themes from the themes folder"
on feature/search
put templates/search.html '<input type="search" placeholder="Search">'
commit priya 10 15:40 "Search box with instant results"
on feature/themes
put themes/dark/site.css 'body { background: #1e1e1e; color: #ddd; }'
commit jonas 11 09:15 "Add a dark theme"
on feature/rss
put quill/feed.py '    # full post content, not just summaries'
commit tomas 11 10:50 "Include full post content in the feed"
on feature/search
put quill/search.py '    # title matches rank above body matches'
commit priya 11 13:30 "Rank title matches above body matches"
git push -q origin feature/search 2>/dev/null
on feature/themes
put quill/themes.py '    # fall back to the default theme'
commit jonas 11 15:45 "Fall back to the default theme when one is missing"
git push -q origin feature/themes 2>/dev/null
on feature/rss
put templates/base.html '<link rel="alternate" type="application/rss+xml" href="/feed.xml">'
commit tomas 12 10:05 "Add a feed link to the page head"
on feature/search
put docs/search.md '# Search
Press / to search the site.'
commit priya 12 11:40 "Document the search shortcut"
on feature/themes
put docs/themes.md '# Themes
Copy themes/dark and make it yours.'
commit priya 12 14:20 "Document how to write a theme"
on main
merge mara 12 16:10 feature/search "Merge branch 'feature/search'"

# A stash, then uncommitted changes.
printf '%s\n' '    # TODO: line numbers in code blocks' >> quill/render.py
git stash push -q -m "line numbers in code blocks (half done)"
printf '%s\n' '- Themes: pick one in quill.toml' >> README.md
printf '%s\n' 'theme = "dark"' > quill.toml
echo "Built $dir: $(git rev-list --all --count) commits; open it with: tortilla log --all $dir"
