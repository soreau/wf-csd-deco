Qt = import( 'qt6' )
QtDeps = dependency(
	'qt6',	
	modules: [ 'Core', 'Gui', 'Quick', 'Qml' ],
	private_headers: true,
)
CoreMocs = Qt.compile_moc(
        headers : [ 'Application.h','DecorationWindow.h' ],
	sources : [ 'main.cpp'],
 	dependencies: QtDeps,
)
qmldeco = executable( 'wf-qml-decorator',
    ['main.cpp', 'protocol.cpp', 'Application.cpp','DecorationWindow.cpp', CoreMocs],
    install: true,
    dependencies: [QtDeps, wayland_client, wf_client_protos]
)
install_data('Border1.qml', install_dir: qml_dir)
install_data('Border2.qml', install_dir: qml_dir)
