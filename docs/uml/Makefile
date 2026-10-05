# Render every diagram to SVG and PNG. PlantUML runs entirely offline.
PUML  := $(filter-out _style.puml,$(wildcard *.puml))
SVG   := $(PUML:.puml=.svg)
PNG   := $(PUML:.puml=.png)

all: $(SVG) $(PNG)

%.svg: %.puml _style.puml
	plantuml -tsvg $<

%.png: %.puml _style.puml
	plantuml -tpng $<

clean:
	rm -f $(SVG) $(PNG)

.PHONY: all clean
