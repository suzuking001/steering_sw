"""Instrument NimBLE 2.5.2 in the build directory for the BLE diagnostic target."""
Import("env")
from pathlib import Path


def insert(source, anchor, replacement):
    if source.count(anchor) != 1:
        raise RuntimeError(f"NimBLE trace hook no longer matches: {anchor.strip()}")
    return source.replace(anchor, replacement)


def instrument(build_env, node):
    original = Path(node.srcnode().get_abspath())
    source = original.read_text()
    trace_header = Path(env.subst("$PROJECT_DIR")) / "include" / "ble_trace.h"
    source = source.replace('#include "ble_hs_priv.h"',
                            f'#include "{original.parent / "ble_hs_priv.h"}"\n#include "{trace_header}"')
    if original.name == "ble_sm.c":
        source = source.replace('#include "ble_hs_resolv_priv.h"',
                                f'#include "{original.parent / "ble_hs_resolv_priv.h"}"')
        source = insert(source, "    /* Strip L2CAP SM header from the front of the mbuf. */",
                        "    sw_ble_trace(SW_BLE_SMP_RX, conn_handle, op, 0);\n\n"
                        "    /* Strip L2CAP SM header from the front of the mbuf. */")
        source = insert(source, "    /* For encrypted state: read LE-encryption bit; ignore BR/EDR and reserved",
                        "    sw_ble_trace(SW_BLE_CONTROLLER_ENCRYPTION, le16toh(ev->connection_handle),\n"
                        "                 ev->enabled & 0x01, ev->status);\n\n"
                        "    /* For encrypted state: read LE-encryption bit; ignore BR/EDR and reserved")
        source = insert(source, "                                           peer_id_addr, &value_sec);\n            if (store_rc == 0)",
                        "                                           peer_id_addr, &value_sec);\n"
                        "            sw_ble_trace(SW_BLE_LTK_LOOKUP, conn_handle, 0, store_rc);\n"
                        "            if (store_rc == 0)")
        source = insert(source, "    uint16_t conn_handle = le16toh(ev->conn_handle);\n\n    memset(&res, 0, sizeof res);",
                        "    uint16_t conn_handle = le16toh(ev->conn_handle);\n"
                        "    sw_ble_trace(SW_BLE_LTK_REQUEST, conn_handle, 0, 0);\n\n"
                        "    memset(&res, 0, sizeof res);")
        source = insert(source, "        ble_gap_enc_event(proc->conn_handle, BLE_HS_ETIMEOUT, 0, 0);",
                        "        sw_ble_trace(SW_BLE_SMP_TIMEOUT, proc->conn_handle, proc->state, proc->rx_key_flags);\n"
                        "        ble_gap_enc_event(proc->conn_handle, BLE_HS_ETIMEOUT, 0, 0);")
    else:
        source = insert(source, "    STATS_INC(ble_l2cap_stats, sm_tx);",
                        "    uint8_t opcode = 0;\n"
                        "    os_mbuf_copydata(txom, 0, 1, &opcode);\n"
                        "    STATS_INC(ble_l2cap_stats, sm_tx);")
        source = insert(source, "    return rc;", "    sw_ble_trace(SW_BLE_SMP_TX, conn_handle, opcode, rc);\n    return rc;")
    target = Path(env.subst("$BUILD_DIR")) / "ble_security_trace" / original.name
    target.parent.mkdir(parents=True, exist_ok=True)
    if not target.exists() or target.read_text() != source:
        target.write_text(source)
    return build_env.File(str(target))


for filename in ("ble_sm.c", "ble_sm_cmd.c"):
    env.AddBuildMiddleware(instrument, f"*/NimBLE-Arduino/src/nimble/nimble/host/src/{filename}")
