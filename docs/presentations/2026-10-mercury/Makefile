# Build the presentation: make (mercury.pdf), make clean, make distclean.
# Needs pdflatex with beamer, the metropolis theme, FiraSans, pgfplots and
# fontawesome5 (Debian: texlive-latex-extra, texlive-fonts-extra,
# texlive-pictures).

DECK    = mercury
LATEX   = pdflatex
LFLAGS  = -interaction=nonstopmode -halt-on-error -file-line-error

all: $(DECK).pdf

# Twice: beamer's navigation and the page count need a second pass.
$(DECK).pdf: $(DECK).tex $(wildcard img/*)
	$(LATEX) $(LFLAGS) $(DECK).tex > /dev/null || { tail -30 $(DECK).log; exit 1; }
	$(LATEX) $(LFLAGS) $(DECK).tex > /dev/null || { tail -30 $(DECK).log; exit 1; }

clean:
	rm -f $(DECK).aux $(DECK).log $(DECK).nav $(DECK).out $(DECK).snm $(DECK).toc $(DECK).vrb

distclean: clean
	rm -f $(DECK).pdf

.PHONY: all clean distclean
