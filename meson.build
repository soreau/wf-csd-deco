project(
	'wf-decorator',
    'c',
	'cpp',
	version: '0.0',
	license: 'MIT',
	meson_version: '>=0.51.0',
	default_options: [
		'cpp_std=c++17',
        'c_std=c99',
		'warning_level=2',
		'werror=false',
	],
)

gtk4 = dependency('gtk4')
wayfire = dependency('wayfire')
wayland_client = dependency('wayland-client')
wayland_server = dependency('wayland-server')

project_args = ['-DWLR_USE_UNSTABLE']
add_project_arguments(project_args, language: ['cpp', 'c'])

subdir('proto')
subdir('wf-gtk4-client')
subdir('wf-plugin')
