#pragma once

#include <net/ieee8023ad_lacp.h>

char *lacp_format_peer(struct lacp_opreq *req, const char *sep);