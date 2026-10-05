# VWS - Virtual Weather Station
#
#   make            build the kernel module and both user-space binaries
#   make kernel     build vws.ko only
#   make user       build vwsd and vwsctl only
#   make load       insert the module and create /dev/vws (needs root)
#   make unload     remove the module (needs root)
#   make unit       run the offline unit tests (no module needed)
#   make test       run the full end-to-end self-test (module must be loaded)
#   make demo       narrated walk through every failure mode
#   make docs       re-render the UML diagrams and recapture the screenshots
#   make clean      remove every build artefact

.PHONY: all kernel user load unload reload demo test unit docs clean

all: kernel user

kernel:
	$(MAKE) -C kernel

user:
	$(MAKE) -C user

load: kernel
	./scripts/load.sh

unload:
	./scripts/unload.sh

reload: unload load

demo: all
	./scripts/demo.sh

# Everything that does not need /dev/vws: filters, health machine, derived
# metrics, fusion. Runs without root and without the module loaded.
unit: user
	./user/vwstest

test: all
	./scripts/selftest.sh

# Regenerate every documentation artefact. The diagrams need plantuml; the
# screenshots need python3-pyte and a loaded module, since they are captured
# from the running programs rather than drawn by hand.
docs: user
	$(MAKE) -C docs/uml
	./scripts/screenshots.sh

docs-screenshots: user
	./scripts/screenshots.sh

clean:
	$(MAKE) -C kernel clean
	$(MAKE) -C user clean
