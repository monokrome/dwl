/* Monitor config for sirius: HDMI 4K on left, eDP laptop on right */
static const MonitorRule monrules[] = {
	/* name       mfact  nmaster scale layout       rotate/reflect                x    y */
	{ "HDMI-A-1", 0.55f, 1,      1.5,  &layouts[0], WL_OUTPUT_TRANSFORM_NORMAL,   0,   0 },
	{ "eDP-1",    0.55f, 1,      1.5,  &layouts[0], WL_OUTPUT_TRANSFORM_NORMAL,   2560, 0 },
	{ NULL,       0.55f, 1,      1,    &layouts[0], WL_OUTPUT_TRANSFORM_NORMAL,   -1,  -1 },
};
