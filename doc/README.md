### Creating PDF Documents

The `*.md` files can easily be converted to PDF documents using *Pandoc*.
To convert e.g. the document *Internals.md* use the command[^1]:

```bash
pandoc -f gfm --mathjax --syntax-definition=../editors/kate/volvox.xml \
       --highlight-style=pygments -s -o Internals.pdf Internals.md
```

Of cause, can use other highlight styles &mdash; e.g. to get the same
highlighting as in the Kate editor use `--highlight-style=kate`.

[^1]: You must have Pandoc and pdfLaTeX installed and the
respective executable directories must have been added to the PATH
environment variable for this to work.
