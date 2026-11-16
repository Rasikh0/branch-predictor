# Workloads for the experiments (spec Section 3.4).
# -O2 because real programs are optimized; -no-pie for fixed addresses, so
# results repeat exactly between runs (together with setarch -R).
CC       = gcc
CFLAGS   = -O2 -g -no-pie
PROGRAMS = loop sort recurse interp gen_text

# The nontrivial program: GNU gzip, built from a fixed source release (rather
# than the system's binary) so every machine runs the same code, compiled the
# same way as the other workloads and with symbols for the per-branch profile.
GZIP_VERSION = 1.12
GZIP_SHA256  = ce5e03e519f637e1f814011ace35c4f87b33c0bbabeec35baf5fbd3479e91956
GZIP_URLS    = http://archive.ubuntu.com/ubuntu/pool/main/g/gzip/gzip_$(GZIP_VERSION).orig.tar.xz \
               https://ftp.gnu.org/gnu/gzip/gzip-$(GZIP_VERSION).tar.xz
GZIP_TAR     = gzip-$(GZIP_VERSION).tar.xz
GZIP_DIR     = gzip-$(GZIP_VERSION)

all: $(PROGRAMS) gzip_input.txt $(GZIP_DIR)/gzip

%: %.c rng.h
	$(CC) $(CFLAGS) $< -o $@

# Input for the gzip workload: 500 KB of generated text, identical every time.
gzip_input.txt: gen_text
	./gen_text 500000 > $@

# Download (trying each URL in turn), then verify the checksum.
$(GZIP_TAR):
	@for url in $(GZIP_URLS); do \
	  echo "Downloading $$url"; \
	  wget -q -O $@.tmp "$$url" && break; \
	done; \
	echo "$(GZIP_SHA256)  $@.tmp" | sha256sum -c --quiet - && mv $@.tmp $@

$(GZIP_DIR)/gzip: $(GZIP_TAR)
	rm -rf $(GZIP_DIR)
	tar xJf $(GZIP_TAR)
	@echo "Building gzip $(GZIP_VERSION) (about 20 s)"
	cd $(GZIP_DIR) && ./configure --quiet CFLAGS="-O2 -g -fno-pie" LDFLAGS="-no-pie" \
	  > configure.log 2>&1 && $(MAKE) -s > make.log 2>&1

clean:
	rm -rf $(PROGRAMS) gzip_input.txt $(GZIP_DIR) $(GZIP_TAR) $(GZIP_TAR).tmp

.PHONY: all clean
