ensuregroup_cmd = config_host['MKVENV_ENSUREGROUP'].split()
pyvenv_common_deps = files(
    meson.project_source_root() + '/pythondeps.toml',
    meson.project_source_root() + '/python/scripts/mkvenv.py'
)
pyvenv_wheel_dir = meson.project_source_root() + '/python/wheels'
pyvenv_functests_group = custom_target(
    'pyvenv_functests_group',
    output: 'functests.group',
    input: pyvenv_common_deps,
    command: ensuregroup_cmd + [
        '--online',
        '--dir', pyvenv_wheel_dir,
        '@INPUT0@',
        'functests',
    ],
)