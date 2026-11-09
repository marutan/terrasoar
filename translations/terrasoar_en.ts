<?xml version="1.0" encoding="utf-8"?>
<!DOCTYPE TS>
<TS version="2.1" language="en" sourcelanguage="en">
    <context>
        <name>main</name>
        <message>
            <source>A 3D globe and 2D map viewer.</source>
            <translation>A 3D globe and 2D map viewer.</translation>
        </message>
        <message>
            <source>Use the translation for &lt;locale&gt; instead of the system language.</source>
            <translation>Use the translation for &lt;locale&gt; instead of the system language.</translation>
        </message>
        <message>
            <source>locale</source>
            <translation>locale</translation>
        </message>
        <message>
            <source>Show every text in a generated test language.</source>
            <translation>Show every text in a generated test language.</translation>
        </message>
        <message>
            <source>Use software OpenGL rendering.</source>
            <translation>Use software OpenGL rendering.</translation>
        </message>
        <message>
            <source>Check that an OpenGL 3.3 context can be created, print the result as JSON and exit.</source>
            <translation>Check that an OpenGL 3.3 context can be created, print the result as JSON and exit.</translation>
        </message>
        <message>
            <source>Open the main window, close it again and exit.</source>
            <translation>Open the main window, close it again and exit.</translation>
        </message>
        <message>
            <source>Measure start-up time and memory, write them as JSON to &lt;file&gt; (- for standard output) and exit.</source>
            <translation>Measure start-up time and memory, write them as JSON to &lt;file&gt; (- for standard output) and exit.</translation>
        </message>
        <message>
            <source>file</source>
            <translation>file</translation>
        </message>
        <message>
            <source>With --measure: stay idle for &lt;seconds&gt; after start-up and report the CPU used meanwhile.</source>
            <translation>With --measure: stay idle for &lt;seconds&gt; after start-up and report the CPU used meanwhile.</translation>
        </message>
        <message>
            <source>seconds</source>
            <translation>seconds</translation>
        </message>
        <message>
            <source>--idle-seconds needs a whole number of seconds.
</source>
            <translation>--idle-seconds needs a whole number of seconds.
</translation>
        </message>
    </context>
    <context>
        <name>ts</name>
        <message>
            <source>Terra Soar</source>
            <comment>Program name; normally kept as it is</comment>
            <translation>Terra Soar</translation>
        </message>
    </context>
    <context>
        <name>ts::BuiltinLayers</name>
        <message>
            <source>Checkerboard</source>
            <translation>Checkerboard</translation>
        </message>
        <message>
            <source>White, 50 %</source>
            <translation>White, 50 %</translation>
        </message>
        <message>
            <source>Blue Marble</source>
            <translation>Blue Marble</translation>
        </message>
        <message>
            <source>Shoreline</source>
            <translation>Shoreline</translation>
        </message>
        <message>
            <source>Land</source>
            <translation>Land</translation>
        </message>
        <message>
            <source>Graticule</source>
            <translation>Graticule</translation>
        </message>
    </context>
    <context>
        <name>ts::GeoFormat</name>
        <message>
            <source>%1° S</source>
            <translation>%1° S</translation>
        </message>
        <message>
            <source>%1° N</source>
            <translation>%1° N</translation>
        </message>
        <message>
            <source>%1° W</source>
            <translation>%1° W</translation>
        </message>
        <message>
            <source>%1° E</source>
            <translation>%1° E</translation>
        </message>
        <message>
            <source>%1 m</source>
            <translation>%1 m</translation>
        </message>
        <message>
            <source>%1 km</source>
            <translation>%1 km</translation>
        </message>
    </context>
    <context>
        <name>ts::GlProbe</name>
        <message>
            <source>Could not create an OpenGL context.</source>
            <translation>Could not create an OpenGL context.</translation>
        </message>
        <message>
            <source>Could not make the OpenGL context current.</source>
            <translation>Could not make the OpenGL context current.</translation>
        </message>
        <message>
            <source>Got an OpenGL ES context; desktop OpenGL is needed.</source>
            <translation>Got an OpenGL ES context; desktop OpenGL is needed.</translation>
        </message>
        <message>
            <source>OpenGL %1.%2 is needed but only %3.%4 is available (%5).</source>
            <translation>OpenGL %1.%2 is needed but only %3.%4 is available (%5).</translation>
        </message>
    </context>
    <context>
        <name>ts::GlRenderDevice</name>
        <message>
            <source>The OpenGL context is not current.</source>
            <translation>The OpenGL context is not current.</translation>
        </message>
        <message>
            <source>OpenGL 3.3 core profile is needed; this context is OpenGL %1.%2.</source>
            <translation>OpenGL 3.3 core profile is needed; this context is OpenGL %1.%2.</translation>
        </message>
        <message>
            <source>The OpenGL 3.3 functions could not be loaded.</source>
            <translation>The OpenGL 3.3 functions could not be loaded.</translation>
        </message>
    </context>
    <context>
        <name>ts::GlobeView</name>
        <message>
            <source>Renderer: %1</source>
            <translation>Renderer: %1</translation>
        </message>
        <message>
            <source>CPU per frame: %1 ms mean, %2 ms max</source>
            <translation>CPU per frame: %1 ms mean, %2 ms max</translation>
        </message>
        <message>
            <source>While moving: %1 frames per second</source>
            <translation>While moving: %1 frames per second</translation>
        </message>
        <message>
            <source>Frames drawn: %1</source>
            <translation>Frames drawn: %1</translation>
        </message>
        <message>
            <source>Target: %1, %2</source>
            <translation>Target: %1, %2</translation>
        </message>
        <message>
            <source>Range: %1, heading %2°, tilt %3°</source>
            <translation>Range: %1, heading %2°, tilt %3°</translation>
        </message>
        <message>
            <source>Eye height: %1</source>
            <translation>Eye height: %1</translation>
        </message>
        <message>
            <source>The globe could not be drawn.</source>
            <translation>The globe could not be drawn.</translation>
        </message>
        <message>
            <source>The globe needs OpenGL 3.3, which is not available:
%1</source>
            <translation>The globe needs OpenGL 3.3, which is not available:
%1</translation>
        </message>
        <message>
            <source>Tiles: %1 drawn (deepest level %2), %3 on the GPU (%4 MB), %5 wanted</source>
            <translation>Tiles: %1 drawn (deepest level %2), %3 on the GPU (%4 MB), %5 wanted</translation>
        </message>
        <message>
            <source>Vectors: %1 tiles drawn, %2 triangles, %3 segments, %4 on the GPU (%5 MB)</source>
            <translation>Vectors: %1 tiles drawn, %2 triangles, %3 segments, %4 on the GPU (%5 MB)</translation>
        </message>
    </context>
    <context>
        <name>ts::LayerDefinition</name>
        <message>
            <source>A layer definition has no id.</source>
            <translation>A layer definition has no id.</translation>
        </message>
        <message>
            <source>Layer definition %1 has an unknown type "%2".</source>
            <translation>Layer definition %1 has an unknown type "%2".</translation>
        </message>
        <message>
            <source>Layer definition %1 has an unknown origin "%2".</source>
            <translation>Layer definition %1 has an unknown origin "%2".</translation>
        </message>
        <message>
            <source>Layer definition %1 has no source kind.</source>
            <translation>Layer definition %1 has no source kind.</translation>
        </message>
    </context>
    <context>
        <name>ts::LayerTreeDock</name>
        <message>
            <source>Layers</source>
            <translation>Layers</translation>
        </message>
        <message>
            <source>Add &amp;Layer</source>
            <translation>Add &amp;Layer</translation>
        </message>
        <message>
            <source>Add Layer</source>
            <translation>Add Layer</translation>
        </message>
        <message>
            <source>Add &amp;Collection</source>
            <translation>Add &amp;Collection</translation>
        </message>
        <message>
            <source>&amp;Remove</source>
            <translation>&amp;Remove</translation>
        </message>
        <message>
            <source>&amp;Opacity:</source>
            <translation>&amp;Opacity:</translation>
        </message>
        <message>
            <source>Composite as &amp;group</source>
            <translation>Composite as &amp;group</translation>
        </message>
        <message>
            <source>Draw the collection's layers together and fade them as one image. Costs an extra full-screen pass.</source>
            <translation>Draw the collection's layers together and fade them as one image. Costs an extra full-screen pass.</translation>
        </message>
        <message>
            <source>Not available with software rendering: the collection's opacity multiplies into its layers instead.</source>
            <translation>Not available with software rendering: the collection's opacity multiplies into its layers instead.</translation>
        </message>
        <message>
            <source>Solid Colour…</source>
            <translation>Solid Colour…</translation>
        </message>
        <message>
            <source>%1 %</source>
            <translation>%1 %</translation>
        </message>
        <message>
            <source>Layer Colour</source>
            <translation>Layer Colour</translation>
        </message>
        <message>
            <source>Colour %1</source>
            <translation>Colour %1</translation>
        </message>
        <message>
            <source>New Collection</source>
            <translation>New Collection</translation>
        </message>
        <message>
            <source>Vector layers (shorelines, graticule) are always drawn above imagery, in their order in the tree.</source>
            <translation>Vector layers (shorelines, graticule) are always drawn above imagery, in their order in the tree.</translation>
        </message>
    </context>
    <context>
        <name>ts::LayerTreeModel</name>
        <message>
            <source>Unknown layer tree entry "%1".</source>
            <translation>Unknown layer tree entry "%1".</translation>
        </message>
    </context>
    <context>
        <name>ts::MainWindow</name>
        <message>
            <source>&amp;File</source>
            <translation>&amp;File</translation>
        </message>
        <message>
            <source>&amp;Help</source>
            <translation>&amp;Help</translation>
        </message>
        <message>
            <source>&amp;Quit</source>
            <translation>&amp;Quit</translation>
        </message>
        <message>
            <source>Close the program</source>
            <translation>Close the program</translation>
        </message>
        <message>
            <source>&amp;About %1</source>
            <translation>&amp;About %1</translation>
        </message>
        <message>
            <source>Show the program version and licence</source>
            <translation>Show the program version and licence</translation>
        </message>
        <message>
            <source>About &amp;Qt</source>
            <translation>About &amp;Qt</translation>
        </message>
        <message>
            <source>Show the Qt version in use</source>
            <translation>Show the Qt version in use</translation>
        </message>
        <message>
            <source>Ready</source>
            <translation>Ready</translation>
        </message>
        <message>
            <source>About %1</source>
            <translation>About %1</translation>
        </message>
        <message>
            <source>&lt;h3&gt;%1 %2&lt;/h3&gt;&lt;p&gt;A 3D globe and 2D map viewer.&lt;/p&gt;&lt;p&gt;Built with Qt %3 (running on Qt %4) and %5.&lt;/p&gt;&lt;p&gt;This program is free software, licensed under the GNU General Public License, version 2 or later.&lt;/p&gt;</source>
            <translation>&lt;h3&gt;%1 %2&lt;/h3&gt;&lt;p&gt;A 3D globe and 2D map viewer.&lt;/p&gt;&lt;p&gt;Built with Qt %3 (running on Qt %4) and %5.&lt;/p&gt;&lt;p&gt;This program is free software, licensed under the GNU General Public License, version 2 or later.&lt;/p&gt;</translation>
        </message>
        <message>
            <source>&amp;View</source>
            <translation>&amp;View</translation>
        </message>
        <message>
            <source>&amp;Reset View</source>
            <translation>&amp;Reset View</translation>
        </message>
        <message>
            <source>Fly back to the start view</source>
            <translation>Fly back to the start view</translation>
        </message>
        <message>
            <source>&amp;Debug Overlay</source>
            <translation>&amp;Debug Overlay</translation>
        </message>
        <message>
            <source>Show frame times and the camera position over the globe</source>
            <translation>Show frame times and the camera position over the globe</translation>
        </message>
        <message>
            <source>The globe needs OpenGL 3.3, which this display does not offer. Start %1 with --software-gl to use software rendering.</source>
            <translation>The globe needs OpenGL 3.3, which this display does not offer. Start %1 with --software-gl to use software rendering.</translation>
        </message>
        <message>
            <source>%1  %2</source>
            <translation>%1  %2</translation>
        </message>
        <message>
            <source>Pointer not on the globe</source>
            <translation>Pointer not on the globe</translation>
        </message>
        <message>
            <source>Eye height %1</source>
            <translation>Eye height %1</translation>
        </message>
        <message>
            <source>&amp;Open Project…</source>
            <translation>&amp;Open Project…</translation>
        </message>
        <message>
            <source>Replace the layers and view with a saved project</source>
            <translation>Replace the layers and view with a saved project</translation>
        </message>
        <message>
            <source>&amp;Save Project</source>
            <translation>&amp;Save Project</translation>
        </message>
        <message>
            <source>Save the layers and view</source>
            <translation>Save the layers and view</translation>
        </message>
        <message>
            <source>Save Project &amp;As…</source>
            <translation>Save Project &amp;As…</translation>
        </message>
        <message>
            <source>Save the layers and view to a new file</source>
            <translation>Save the layers and view to a new file</translation>
        </message>
        <message>
            <source>Open Project</source>
            <translation>Open Project</translation>
        </message>
        <message>
            <source>Terra Soar projects (*.terrasoar);;All files (*)</source>
            <translation>Terra Soar projects (*.terrasoar);;All files (*)</translation>
        </message>
        <message>
            <source>Save Project</source>
            <translation>Save Project</translation>
        </message>
        <message>
            <source>Terra Soar projects (*.terrasoar)</source>
            <translation>Terra Soar projects (*.terrasoar)</translation>
        </message>
        <message>
            <source>The base pack is not installed; showing test imagery.</source>
            <translation>The base pack is not installed; showing test imagery.</translation>
        </message>
        <message>
            <source>The base pack is damaged (%1). Reinstall %2 to repair it.</source>
            <translation>The base pack is damaged (%1). Reinstall %2 to repair it.</translation>
        </message>
    </context>
    <context>
        <name>ts::ProjectFile</name>
        <message>
            <source>The file is not valid JSON: %1</source>
            <translation>The file is not valid JSON: %1</translation>
        </message>
        <message>
            <source>The file is not a project.</source>
            <translation>The file is not a project.</translation>
        </message>
        <message>
            <source>The project has no version.</source>
            <translation>The project has no version.</translation>
        </message>
        <message>
            <source>The project was saved by a newer version of the program (format %1; this one reads up to %2).</source>
            <translation>The project was saved by a newer version of the program (format %1; this one reads up to %2).</translation>
        </message>
        <message>
            <source>Could not write %1: %2</source>
            <translation>Could not write %1: %2</translation>
        </message>
        <message>
            <source>Could not read %1: %2</source>
            <translation>Could not read %1: %2</translation>
        </message>
    </context>
    <context>
        <name>ts::ProviderRegistry</name>
        <message>
            <source>Unknown or invalid imagery source: %1</source>
            <translation>Unknown or invalid imagery source: %1</translation>
        </message>
        <message>
            <source>The base pack, which holds "%1", is not installed.</source>
            <translation>The base pack, which holds "%1", is not installed.</translation>
        </message>
        <message>
            <source>Cannot read "%1" from the base pack: %2</source>
            <translation>Cannot read "%1" from the base pack: %2</translation>
        </message>
        <message>
            <source>Unknown or invalid feature source: %1</source>
            <translation>Unknown or invalid feature source: %1</translation>
        </message>
    </context>
    <context>
        <name>ts::TestProviders</name>
        <message>
            <source>Solid colour %1</source>
            <translation>Solid colour %1</translation>
        </message>
        <message>
            <source>Checkerboard</source>
            <translation>Checkerboard</translation>
        </message>
    </context>
</TS>
