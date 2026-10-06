"""Turn spec/vectors/lanicom-v1.json into a C header for the host tests."""

import json
import sys


def c_bytes(hexstr):
    data = bytes.fromhex(hexstr)
    return "{" + ",".join(f"0x{b:02x}" for b in data) + "}" if data else "{0}", len(data)


def c_str(s):
    # Every byte as \xNN: safe for any UTF-8, and no escape can swallow the next character.
    return '"' + "".join(f"\\x{b:02x}" for b in s.encode()) + '"'


def target(t):
    if "device" in t:
        return f"{{LC_TARGET_DEVICE, 0x{t['device']:08x}u}}"
    return "{LC_TARGET_ALL, 0}"


def main(src, dst):
    v = json.load(open(src))
    out = ["/* Generated from spec/vectors/lanicom-v1.json by gen_vectors_h.py. */", "#pragma once",
           '#include "lanicom/lanicom.h"', ""]

    out.append("typedef struct { const char *key_string; const char *master; uint16_t key_id; "
               "uint32_t sids[3]; const char *send_keys[3]; } key_vec_t;")
    out.append("static const key_vec_t KEY_VECS[] = {")
    for k in v["keys"]:
        sids = list(k["send_keys"].items())
        out.append(f"  {{{c_str(k['key_string'])}, \"{k['master']}\", {k['key_id']}, "
                   f"{{{', '.join('0x' + s + 'u' for s, _ in sids)}}}, {{{', '.join(chr(34) + x + chr(34) for _, x in sids)}}}}},")
    out.append("};")

    out.append("typedef struct { const char *key_string; uint8_t type; uint32_t sender_id; uint64_t epoch; "
               "uint32_t seq; const char *plaintext; const char *packet; } packet_vec_t;")
    out.append("static const packet_vec_t PACKET_VECS[] = {")
    for p in v["packets"]:
        out.append(f"  {{{c_str(p['key_string'])}, {p['type']}, 0x{p['sender_id']:08x}u, 0x{p['epoch']:016x}ull, "
                   f"{p['seq']}u, \"{p['plaintext']}\", \"{p['packet']}\"}},")
    out.append("};")

    out.append("typedef struct { const char *name; const char *key_string; const char *packet; const char *reason; } invalid_vec_t;")
    out.append("static const invalid_vec_t INVALID_VECS[] = {")
    for p in v["invalid_packets"]:
        out.append(f"  {{\"{p['name']}\", {c_str(p['key_string'])}, \"{p['packet']}\", \"{p['reason']}\"}},")
    out.append("};")

    out.append("typedef struct { const char *name; const char *bytes; bool decode_only; lc_control_t msg; } control_vec_t;")
    out.append("static const control_vec_t CONTROL_VECS[] = {")
    for c in v["control"]:
        m = c["message"]
        if "hello" in m:
            h = m["hello"]
            body = (f"{{.type = LC_MSG_HELLO, .u.hello = {{{c_str(h.get('name', ''))}, "
                    f"{h.get('caps', 0)}u, 0x{h.get('challenge', 0):016x}ull, 0x{h.get('echo', 0):016x}ull, "
                    f"{'true' if h.get('bye') else 'false'}}}}}")
        elif "talk_start" in m:
            t = m["talk_start"]
            body = (f"{{.type = LC_MSG_TALK_START, .u.talk_start = {{{target(t['target'])}, "
                    f"0x{t.get('stream_id', 0):08x}u}}}}")
        else:
            body = f"{{.type = LC_MSG_TALK_STOP, .u.talk_stop = {{0x{m['talk_stop']['stream_id']:08x}u}}}}"
        out.append(f"  {{\"{c['name']}\", \"{c['bytes']}\", {'true' if c.get('decode_only') else 'false'}, {body}}},")
    out.append("};")

    out.append("typedef struct { uint32_t first; size_t n; uint32_t seq[16]; bool ok[16]; } replay_vec_t;")
    out.append("static const replay_vec_t REPLAY_VECS[] = {")
    for r in v["replay"]:
        seqs = ", ".join(f"0x{s:08x}u" for s, _ in r["steps"])
        oks = ", ".join("true" if ok else "false" for _, ok in r["steps"])
        out.append(f"  {{0x{r['first']:08x}u, {len(r['steps'])}, {{{seqs}}}, {{{oks}}}}},")
    out.append("};")
    open(dst, "w").write("\n".join(out) + "\n")


if __name__ == "__main__":
    main(*sys.argv[1:3])
