# Tool for viewing and searching docs for DuckDB

<img width="500" height="273" alt="logo" src="logo.jpg" />

A standalone program for using semantic search on DuckDB's manual and blog:

```bash
docs "How to query a parquet file?"
docs "How to query a csv file?"
```

It offers an interactive terminal UI and a non-interactive mode which can be
used by LLMs to extend their knowledge with the latest version of DuckDB.
With the integration of Llama.cpp, questions can also be answered with an LLM.

## How it works

The binary loads the Markdown sources that sit next to it and builds compact
index files in the same directory on first use. Every query is answered with
semantic search: the Markdown is embedded with a small embedding model (bge-small-en)
through Llama.cpp and searched through an HNSW vector index, so a question such as
"how to read a parquet file?" finds the same pages as the keyword "parquet".
A search covers the documentation and the blog together.

The program looks for these files in its own directory:

```text
docs                                the binary (includes the embedding model)
duckdb-docs.md                      documentation text
duckdb-blog.md                      blog text
```

If either file is missing, it is downloaded automatically from
`https://blobs.duckdb.org/docs/duckdb-blog.md` or `https://blobs.duckdb.org/docs/duckdb-docs.md`
at startup. `docs --update` downloads both files again even when they
already exist. To answer a question with an LLM, the LLM file is downloaded once on
the first attempt of `Answer with LLM` from Huggingface and cached locally in the
huggingface cache folder.

Generated files are written next to the binary, so the whole installation stays
in one directory. The cache is invalidated with checksums of the source files,
so updating a Markdown file rebuilds the indexes automatically.
Generated files can be deleted at any time.

## Using it from the DuckDB shell

The DuckDB shell's `.system` command runs an external program and prints its
output. Put the built program on `PATH`:

```sh
export PATH="/path/to/docs-extension/build/release:$PATH"
```

Then, from inside `duckdb`:

```text
.system docs "How to query a parquet file?"
.system docs "How to query a csv file?"
```

## Using it by an LLM (tool calling)

```bash
docs --raw --snippets "<keyword-or-question>" 1
```

## Usage

```bash
docs [options] [query] [N]
```

The default limit is 10 and the maximum is 1000. `--limit 3` and a trailing `3`
are equivalent:

```bash
docs "how to read a parquet file" 3
docs --limit 3 "how to read a parquet file"
```

Every search covers the documentation and the blog together.
By default, results are complete pages: all sections that belong to the same
documentation page or blog post are grouped into a single result, and the whole
page is rendered, including its subsections. The limit counts pages. Pass
`--snippets` to return the best-matching content block of each page instead of
complete pages.

Options:

```text
-l, --limit N          Maximum number of results (default 10, max 1000)
-u, --update           Download both Markdown files again and exit
    --color[=WHEN]     Colorize output: auto (default), always, never
    --no-color         Disable colors (same as --color=never)
    --raw              Print the original Markdown without rendering
    --no-ui            Print results without interactive UI
    --snippets         Print content snippets instead of complete pages (implies --no-ui)
    --llm              Answer with the local LLM instead of listing results
    --model=REPO       HuggingFace repo for LLM answer (default openbmb/MiniCPM5-1B-GGUF:Q4_K_M)
-h, --help             Show this help
```

The program exits with status 0 on a successful search, including when there
are no matches, and with a non-zero status on invalid arguments or an internal
error.

## Live search

Running the program with no query on a terminal opens a full-screen search line
at the top:

```text
search> parquet

1 Reading and Writing Parquet Files
2 Querying Parquet Metadata
3 Blog: Query Engines: Gatekeepers of the Parquet File Format
4 Parquet Encryption
5 Parquet Tips
...
11 Answer with LLM
12 Quit
```

The empty prompt lists the ten latest blog articles. The search runs again on
every keystroke. Up/Down selects an entry, Enter opens the full-screen pager at
the selected result, and a mouse click on a result opens it directly. The list
ends with `Answer with LLM` and `Quit` entries. Select `Answer with LLM` and
press Enter to ask the local model about the current query; `Quit` exits the
application. ESC quits.
Pass `--no-ui` or redirect the output to get the usage message instead.

The same search line is available from inside the pager: press ESC to edit the
current query, type a new one, and press Enter to apply it. ESC in the search
prompt exits the application.

To look for text inside the page that is currently open, press `/` and type a
pattern. Matching text is highlighted and the view jumps to the first match.
While a page search is active, `n` and `p` jump to the next and previous match.

## Answer with LLM

Pass `--llm` to answer a query directly with an LLM instead of listing results:

```sh
docs --llm "how do I read a parquet file?"
```

On a terminal, `--llm` opens a live answer view: the reasoning is shown while
the model thinks and hidden once the answer starts, and the finished answer can
be scrolled. When the output is not a terminal, reasoning is streamed under a
`Reasoning` heading followed by the answer under an `Answer` heading.
Load and prompt progress go to standard error. The flag requires a query.

Select `Answer with LLM` in the result list and press Enter. The first use loads
the local answer model. By default this is MiniCPM5.
When the local model is missing, it is downloaded
from HuggingFace through llama.cpp's downloader and stored in the standard
HuggingFace cache (`~/.cache/huggingface/hub`). The model stays loaded for the
rest of the session.

Pass `--model=<repo>` to answer with another GGUF model from HuggingFace
instead. The value is a model repository, optionally followed by a quantization
tag, for example:

```sh
docs --llm --model=<user>/<model>-GGUF:Q4_K_M "how do I read a parquet file?"
```

In the live search, `--model` also controls the model used by `Answer with LLM`.
The program searches the documentation and blog as usual, takes the ten
best-matching content blocks (chunks of at most about 1,800 characters, each
prefixed with its page title), and sends the question together with those blocks
to the answer model. `ESC` stops answer generation.
`Enter` stops generation and starts it again, so a new answer can
be requested without leaving the view. The model's reasoning is streamed below
the list under `Reasoning` and is hidden as soon as the answer starts. The final
answer is streamed under `Answer`.
Use the mouse wheel, PgUp/PgDn, or Home/End to scroll the answer;
Once the answer is finished, `ESC` exits the application.

```text
search> how do I read a parquet file?
...
11 Answer with LLM
12 Quit
```

The answer uses a 16,384-token context. When the ten matching blocks do not fit,
blocks are dropped from the end and the last remaining block is shortened. The
question and the model's generation prompt are always kept. Sampling uses a
temperature of 0.9.

## Semantic search

Every query is embedded and searched by meaning. This handles natural-language
questions that do not contain the right keywords:

```sh
docs "how to read a parquet file?"
docs "parquet"
```

Both commands return the Parquet pages first. Short keyword queries use the same
semantic index; there is no separate lexical ranking.

The semantic index is built from the Markdown the first time it is needed. The
embedding model is `BAAI/bge-small-en-v1.5`, embedded in the binary and loaded
through llama.cpp. Every page is split into chunks of at most about 1800 characters,
then embedded with the page title as context. The normalized vectors are stored
in an HNSW index and searched with cosine similarity. Results are grouped back
into complete pages, so several matching chunks from the same page still produce
one result. The HNSW index and its metadata are cached next to the other generated
files:

```text
<binary directory>/
  semantic-bge-small-en-v1.5-q8_0.hnsw
  semantic-bge-small-en-v1.5-q8_0.meta
```

The metadata records a SHA-1 fingerprint of the Markdown sources and the
embedded model, so the index is rebuilt automatically when the documentation or
the model changes.

The index is prepared at startup before the search runs, both for a query and
when the interactive search is opened. While it is being built, the terminal
shows a progress line such as:

```text
docs: building semantic index for bge-small-en-v1.5-q8_0 (4824 chunks, 4 threads)...
docs: embedding chunks 1206/4824 (25%, 0:23 elapsed)
```

Index builds use four CPU threads and offload the model to the GPU if possible,
which keeps the rest of the system responsive. Later searches load the saved HNSW
index in a fraction of a second.

The embedding model is `BAAI/bge-small-en-v1.5`. The model bytes are
linked into the binary, so the release is a single self-contained executable:
there is no model file to copy. The llama.cpp submodule is also linked
statically. The only runtime dependency is the platform GPU stack: the Vulkan
loader on Linux and Windows, and Metal on macOS. Refresh the embedding model if
necessary before building:

```sh
wget https://huggingface.co/ggml-org/bge-small-en-v1.5-Q8_0-GGUF/resolve/main/bge-small-en-v1.5-q8_0.gguf
```

## Search quality

Semantic search is meant for natural-language phrasing rather than exact
keywords. For example, `how to read a parquet file?` returns the Parquet pages
at the top, matching the keyword search for `parquet`:

```text
Parquet Files, Reading and Writing Parquet Files, ...
```

Short keyword queries also get exact-term handling:

- Unknown words are corrected against a vocabulary built from the Markdown, so
  `parket` searches for `parquet` and `varkar` searches for `varchar`. When a
  correction changes the query, a `corrected to: parquet` line is shown above
  the first result.
- Queries of up to three words get a small boost for pages whose title or body
  contains the corrected terms. This keeps `varchar` on the `Text Types` page
  and `csv` on `CSV Files`.

Longer natural-language queries skip the exact-term boost, so they are ranked
purely by embedding similarity.
The embedding context is capped at the model's 512-token training
window, while the batch and ubatch are both 2048 tokens.

## Paging results

On an interactive terminal the pager takes over the whole terminal using the
alternate screen buffer. The first line is a tab bar with the titles of the
results and the active result highlighted. Pressing
`Ctrl+c` restores the terminal contents that were visible before the search started.
Pressing ESC returns to the search prompt with the current query. If not all titles fit,
the tab bar shows a window around the active title with `…` markers:

```text
 Search | Text Types | Types API | Time Types | Relational API | CREATE MACRO Statement
----------------------------------------------------------------------------------------
...
 Result 1/10  |  Lines 1-24/80  |  ESC Search  |  / Find-in-page  |  Up/Down scroll  |  Left/Right prev,next
```

When the terminal is wide enough, rendered pages also show an outline of their
section headings on the right. The outline follows the page as you scroll and
highlights the section at the top of the viewport. Click an outline entry to
jump to that section; the outline itself scrolls when a page has more headings
than fit on screen.

Keys:

```text
Up, Down, j, k          Scroll one line
Mouse wheel             Scroll three lines
PageUp, PageDown        Scroll one page
g, G, Home, End         Jump to the start or end
Left, p                 Previous result; at the first result, open the search page
Right                   Next result; wraps from the last result to the first
n                       Next result; at the last result, open the search page
Enter                   Go back from an internal page
ESC                     Go back; ESC in the search prompt exits the program
/                       Find text in the current page (Enter repeats)
Ctrl+c                  Quit
```

The pager captures the mouse so that clicks, dragging, and the wheel work. Drag
over the content to copy just the text under the pointer.
Copying uses OSC-52 where the terminal supports it. Otherwise it falls back to
`xclip` or `xsel` on X11, `wl-clipboard` on Wayland, `pbcopy` on macOS, and the
native clipboard on Windows.
The first item in the top line is ` Search`, the search page. Click it with the
mouse to open the search prompt, or click any title to select that result.
Pressing Left on the first result selects ` Search`; Right on the last result
wraps around to the first result. Esc in the search prompt exits the
application; Enter applies the edited query.

Clicking a link in the page content opens it inside the pager when the target
page is part of the bundled documentation or blog. This also applies to links
written for older documentation versions: the local page is shown instead of
the published URL. The internal view has the same scrolling and find-in-page
keys. Press Left/Right to select a link and Enter to open it; Esc clears the
selection. Esc or a click on `[Back]` returns to the previous page when no link
is selected, and `Ctrl+c` quits the application.

Blog pages that are only redirect stubs end with an `Open the original post`
link. The full article lives on the site the DuckDB website redirects to, so the
link opens it in the browser.

Links to other sites open with the desktop's default handler (`xdg-open` or
`gio open` on Linux, `open` on macOS). This uses the pager's own mouse handling,
so content links stay clickable even in terminals whose OSC 8 support is
disabled or broken. Only `http` and `https` targets are opened.

The pager is only used when standard input and standard output are both
terminals and more than one result was found. Otherwise every result is printed
in sequence, which is what happens in pipelines, in scripts, and through
`.system` when its output is redirected. Pass `--no-ui` to
disable paging explicitly. `--limit 1` also prints a single result without the
pager.

## Terminal rendering

Results are complete Markdown pages, so they are rendered for the terminal by
default: headings are colored, emphasis is bold or italic, lists keep their
markers, block quotes get a bar, and tables use box-drawing characters. Code
blocks get a dim `│ ` gutter.

Fenced blocks marked ```` ```sql ```` are syntax
highlighted: keywords, types, strings, numbers, comments, function names and
operators get distinct colors. Highlighting understands line and nested block
comments, single-quoted strings with doubled quotes, quoted identifiers, and
dollar-quoted strings. `--no-color` and `--color=never` produce plain text while
keeping the gutter.

Links in the rendered page are emitted as terminal hyperlinks. The label
is underlined and clickable, but the URL is not printed, which keeps pages
readable. Links to the bundled documentation are blue, while links to other
sites are orange. Relative Markdown targets and bare `#fragment` anchors are resolved to
absolute `https://duckdb.org/...` URLs first, so every link points at the
published page. This works in terminals that support OSC 8 (GNOME Terminal,
iTerm2, Kitty, WezTerm, Alacritty, Windows Terminal, and others). Use `--raw` to
see the original Markdown including link targets. The pager does not depend on
that terminal support: it records the screen span of every link and opens the
target itself when the link is clicked.

The result titles in the pager's top line and the result list in the search
prompt can be selected with the mouse: clicking a title switches to that
result, and clicking a search result opens it. This works in any terminal that
supports standard mouse reporting.

Konsole supports OSC 8 but disables it by default. Enable **Allow escape
sequences for links** under **Settings → Edit Current Profile → Mouse →
Miscellaneous**.

Color and hyperlinks are enabled automatically when standard output is a
terminal. `--color=always` forces them on, and `--no-color` emits plain text with
no ANSI escapes. `NO_COLOR` and `CLICOLOR_FORCE` are honored in `auto` mode.

## Building

Requirements:

- a C99 compiler and a C++17 compiler
- CMake 3.16 or newer
- OpenSSL development headers, used to download the Markdown sources over HTTPS
- the embedding model file
- on Linux and Windows, the Vulkan SDK (including `glslc`)
- on macOS, the Metal toolchain that ships with Xcode

The llama.cpp source in `third_party/llama.cpp` is a git submodule pinned to the
latest stable release. Check it out first:

```sh
git submodule update --init --recursive
```

Docs is compiled as a static library with a GPU backend and linked into the
binary: Vulkan on Linux and Windows, Metal on macOS. The CPU backend is still
available, so systems without a GPU fall back automatically. On Linux the
binary links against the Vulkan loader (`libvulkan.so.1`) and uses the installed
driver at runtime. On x86-64 the build uses `-march=x86-64-v3`, so the resulting
binary requires AVX2 (Haswell or newer); on ARM it targets the `armv8-a`
baseline. The answer model is not part of the build. When it is missing, answer mode
downloads it into the HuggingFace cache on first use.

Build with make:

```sh
make
```

The build produces the binary and copies its source data next to it:

```text
build/release/docs
build/release/duckdb-docs.md
build/release/duckdb-blog.md
build/release/semantic-bge-small-en-v1.5-q8_0.hnsw
build/release/semantic-bge-small-en-v1.5-q8_0.meta
```

The build contains the embedding model, llama.cpp, and the compiled GPU shaders.
The answer model (default MiniCPM5, ~656 MB) is downloaded on demand into the local HuggingFace cache.
The embedding model is linked into the executable; the answer model is loaded from disk when the
answer mode is opened for the first time.

Run it directly:

```sh
./build/release/docs "how to read a parquet file"
./build/release/docs "how to read a csv file"
```

To install it into `~/.local/bin`:

```sh
make install
```

The install step copies the binary, Markdown sources and indexes into `$HOME/.local/bin`.
Make sure that directory is on `PATH`; llama.cpp and the embedding model are linked statically.
The only runtime requirement beyond those files is the platform GPU stack.
After that, `.system docs "..."` works directly inside the DuckDB shell.

## How the data is built

The source files live in the project root:

```text
duckdb-docs.md
duckdb-blog.md
```

At run time the binary looks for them next to itself. When a file is missing,
it is fetched from `https://blobs.duckdb.org/docs/duckdb-docs.md` or
`https://blobs.duckdb.org/docs/duckdb-blog.md` and written to the folder of
the binary. `docs --update` re-downloads both files.
On first use, the program splits each Markdown file at headings (`#` through
`######`) and writes section metadata plus a copy of the normalized text. These
files are written next to the binary:

```text
<binary directory>/
  duckdb-docs-data
  duckdb-docs-index
  duckdb-blog-data
  duckdb-blog-index
  duckdb-vocabulary
  semantic-bge-small-en-v1.5-q8_0.hnsw
  semantic-bge-small-en-v1.5-q8_0.meta
```

Each index stores the SHA-1 checksum of its Markdown source. If a source file
changes, the next run rebuilds the affected cache files automatically. The data
file is also compared with the normalized Markdown, so a corrupted or truncated
data file is regenerated. The vocabulary cache stores a word frequency table
built from both Markdown files and is rebuilt when their combined fingerprint
changes. The semantic metadata stores a combined SHA-1 fingerprint of both
Markdown files and the embedded model, and the HNSW file is rebuilt when it does
not match. The generated files can be deleted at any time; they are rebuilt on
the next search.

The local section index stores each heading's title, anchor, level, start line,
line count, body length and whether it has body text. It is not a search index:
ranking comes entirely from the semantic vectors. The vocabulary cache stores
word frequencies for spelling correction; it is not an inverted index and does
not contain page references. The Markdown data files contain the page text that
is rendered in full. The semantic index stores only vectors and a page id per
chunk; result text and titles still come from the Markdown data files.

## Search behaviour

Every query goes through the semantic index described above. At query time the
program:

1. Tokenizes the query and corrects unknown words against the vocabulary. A word
   is replaced only when a candidate is within one edit (two for words of six or
   more characters), starts with the same letter, and occurs at least twice in
   the documentation.
2. Embeds the corrected query with the same embedding model and the BGE query prefix.
3. Searches the HNSW index for the nearest chunks with cosine similarity.
4. Groups the chunks back into complete pages. A page's score is the highest
   score of its chunks, so several matching chunks from the same page still
   produce one result.
5. For queries of up to three words, adds a small exact-term boost based on
   occurrences in the page title and normalized by the page length.
6. Returns the top complete pages, which are rendered from the local Markdown.
   Blog pages are titled with a `Blog: ` prefix so they are easy to spot in
   combined results.
7. Spelling correction handles common typos such as `parket` and `varkar`.

## Development

```bash
clang-format-20 -i src/**/*.cpp src/**/*.hpp
intercept-build-20 make
run-clang-tidy-20 -p build/release -quiet -j$(nproc) -source-filter $(pwd)/src/
```

## AI Disclosure

The code is written by AI, tested manually.

## License

MIT License
