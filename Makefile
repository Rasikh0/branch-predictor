# Top-level build.
#   make           Pin tool and directed tests (needs PIN_ROOT)
#   make unittest  predictor unit tests (needs only g++)

all: tool tests

tool:
	@if [ -z "$(PIN_ROOT)" ]; then \
	  echo "PIN_ROOT is not set. Run: export PIN_ROOT=/path/to/pin-kit"; exit 1; fi
	$(MAKE) -C pintool obj-intel64/bpred.so

tests:
	$(MAKE) -C tests

unittest:
	$(MAKE) -C tests/unit all
	./tests/unit/test_predictor

clean:
	rm -rf pintool/obj-intel64
	$(MAKE) -C tests clean
	$(MAKE) -C tests/unit clean

.PHONY: all tool tests unittest clean
