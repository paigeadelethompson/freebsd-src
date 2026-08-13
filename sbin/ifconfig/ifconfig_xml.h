#pragma once

extern int ifconfig_xml_dry_run;

int ifconfig_xml_apply(if_ctx *ctx, const char *filename);
