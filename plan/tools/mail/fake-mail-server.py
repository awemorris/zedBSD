#!/usr/bin/env python3
# zedBSD
# Copyright (C) 2026 Awe Morris
# SPDX-License-Identifier: Zlib
"""A small IMAP4rev1 and SMTP server for Mail's tests (ws169-p003): the host tests (plan/ws169/tests/run-host-mail-backend.sh)
and the AAT helper of tests/scenarios/apps/mailer/ (plan/tools/aat/scenarios/helpers_mailer.py).

    fake-mail-server.py CERT KEY OUTDIR [--bind ADDRESS] [--arrivals N]

Listens on 127.0.0.1 (or ADDRESS: the AAT helper's address the target reaches the host at) with four free ports: IMAP with TLS from the start, IMAP with STARTTLS, SMTP with TLS
from the start and SMTP submission with STARTTLS, and prints "PORTS imaps imap smtps submission" when ready.
One user (kei@example.net, password "secret 1") with INBOX, Sent, Drafts, Archive and Trash (marked with
their special use), three messages in INBOX.  IDLE tells a new message (a sign-in code) 0.5 s after it starts,
the first N times only with --arrivals (Mail idles again after each).  The
messages SMTP receives are written to OUTDIR/smtp-N.eml with their envelope in OUTDIR/smtp-N.env, and the
messages APPENDed to OUTDIR/append-N.eml.  Runs until it is killed.

ws177-p016 (each only when asked, so that the other tests see the server as before):
    --caps "MOVE UIDPLUS"   more words in CAPABILITY (UID MOVE and UID EXPUNGE are always understood)
    --large                 a fourth INBOX message of about 1.5 MiB: a file first, then its words in ISO-2022-JP,
                            for the fetch by BODYSTRUCTURE and BODY.PEEK[section]
    --smtp-auth "LOGIN"     the logins EHLO offers (AUTH PLAIN by default); each login taken is written to
                            OUTDIR/logins (PLAIN or LOGIN, a line each)
    --japanese-folders      the folders without special use, named in Japanese (modified UTF-7), the trash's name
                            sent as a literal
A FETCH honours a partial range <0.N>, BODY.PEEK[HEADER] and BODY.PEEK[1], [2.1] ..., and BODYSTRUCTURE.
"""

import os
import socket
import ssl
import sys
import threading
import time
import base64
import email
import email.policy

USER = "kei@example.net"
PASSWORD = "secret 1"

INBOX = [
    (b"From: Example Bank <no-reply@bank.example>\r\n"
     b"To: kei@example.net\r\n"
     b"Subject: =?UTF-8?B?44K144Kk44Oz44Kk44Oz?= code\r\n"
     b"Date: Mon, 5 Oct 2026 09:41:00 +0900\r\n"
     b"Message-ID: <bank-1@bank.example>\r\n"
     b"MIME-Version: 1.0\r\n"
     b"Content-Type: multipart/alternative; boundary=\"b1\"\r\n"
     b"\r\n"
     b"--b1\r\n"
     b"Content-Type: text/plain; charset=utf-8\r\n"
     b"Content-Transfer-Encoding: quoted-printable\r\n"
     b"\r\n"
     b"Use this code: 482913 =E3=81=A7=E3=81=99=\r\n"
     b"\r\n"
     b"--b1\r\n"
     b"Content-Type: text/html; charset=utf-8\r\n"
     b"\r\n"
     b"<p>Use this code: <b>482913</b></p>\r\n"
     b"--b1--\r\n"),
    (b"From: \"Aiko Tanaka\" <aiko@example.org>\r\n"
     b"To: kei@example.net\r\n"
     b"Subject: Photos\r\n"
     b"Date: Sun, 4 Oct 2026 20:30:00 +0000\r\n"
     b"Content-Type: multipart/mixed; boundary=\"m1\"\r\n"
     b"\r\n"
     b"--m1\r\n"
     b"Content-Type: text/plain\r\n"
     b"\r\n"
     b"Here are the photos.\r\n"
     b"--m1\r\n"
     b"Content-Type: application/zip; name=\"river-walk.zip\"\r\n"
     b"Content-Disposition: attachment; filename=\"river-walk.zip\"\r\n"
     b"Content-Transfer-Encoding: base64\r\n"
     b"\r\n"
     b"UEsDBAoAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA=\r\n"
     b"--m1--\r\n"),
    (b"From: ben@example.com\r\n"
     b"To: kei@example.net\r\n"
     b"Subject: Saturday?\r\n"
     b"Date: Sat, 3 Oct 2026 10:00 -0500\r\n"
     b"\r\n"
     b"Are we on?\r\n"),
]

NEW_MESSAGE = (b"From: Shop <shop@example.com>\r\n"
               b"To: kei@example.net\r\n"
               b"Subject: Your verification\r\n"
               b"Date: Tue, 6 Oct 2026 12:00:00 +0000\r\n"
               b"\r\n"
               b"Order 2026 is ready. Your one-time code is 7351.\r\n")

FOLDERS = [("INBOX", ""), ("Sent", "\\Sent"), ("Drafts", "\\Drafts"), ("Archive", "\\Archive"), ("Trash", "\\Trash")]

# --japanese-folders: the names LIST gives (modified UTF-7 of 送信済み, 下書き, アーカイブ, ごみ箱) for the store's boxes.
JAPANESE_NAMES = {"Sent": "&kAFP4W4IMH8-", "Drafts": "&Tgtm+DBN-", "Archive": "&MKIw,DCrMKQw1g-", "Trash": "&MFQwf3ux-"}
JAPANESE = False
EXTRA_CAPS = ""
SMTP_AUTH = "PLAIN"
LOGINS = []


def large_message():
    """A message of about 1.5 MiB: a file first, then its words in ISO-2022-JP (--large)."""
    words = "大きな添付のメールです。".encode("iso-2022-jp")
    data = base64.encodebytes(bytes(range(256)) * 6144).replace(b"\n", b"\r\n")
    return (b"From: Big <big@example.org>\r\n"
            b"To: kei@example.net\r\n"
            b"Subject: =?ISO-2022-JP?B?" + base64.b64encode("大きな添付".encode("iso-2022-jp")) + b"?=\r\n"
            b"Date: Wed, 7 Oct 2026 08:00:00 +0000\r\n"
            b"MIME-Version: 1.0\r\n"
            b"Content-Type: multipart/mixed; boundary=\"L1\"\r\n"
            b"\r\n"
            b"--L1\r\n"
            b"Content-Type: application/octet-stream; name=\"big.bin\"\r\n"
            b"Content-Disposition: attachment; filename=\"big.bin\"\r\n"
            b"Content-Transfer-Encoding: base64\r\n"
            b"\r\n" + data +
            b"--L1\r\n"
            b"Content-Type: text/plain; charset=iso-2022-jp\r\n"
            b"Content-Transfer-Encoding: 7bit\r\n"
            b"\r\n" + words + b"\r\n"
            b"--L1--\r\n")


def box_name(name):
    """The store's box for a name LIST gave."""
    for box, japanese in JAPANESE_NAMES.items():
        if JAPANESE and name == japanese:
            return box
    return name


def imap_string(text):
    if text is None:
        return "NIL"
    return '"' + text.replace("\\", "\\\\").replace('"', '\\"') + '"'


def body_structure(part):
    """BODYSTRUCTURE of a part of email's message (RFC 3501 7.4.2)."""
    if part.is_multipart():
        inner = "".join(body_structure(child) for child in part.get_payload())
        return "(%s %s)" % (inner, imap_string(part.get_content_subtype().upper()))
    params = []
    for key in ("charset", "name"):
        value = part.get_param(key)
        if value is not None:
            params += [imap_string(key.upper()), imap_string(str(value))]
    params_text = "(" + " ".join(params) + ")" if params else "NIL"
    encoding = part.get("Content-Transfer-Encoding", "7bit").strip().upper()
    body = section_body(part)
    fields = "%s %s %s NIL NIL %s %d" % (imap_string(part.get_content_maintype().upper()),
                                         imap_string(part.get_content_subtype().upper()),
                                         params_text, imap_string(encoding), len(body))
    if part.get_content_maintype() == "text":
        fields += " %d" % body.count(b"\n")
    disposition = "NIL"
    if part.get_content_disposition() is not None:
        filename = part.get_param("filename", header="content-disposition")
        disposition = "(%s %s)" % (imap_string(part.get_content_disposition().upper()),
                                   "(" + imap_string("FILENAME") + " " + imap_string(str(filename)) + ")" if filename else "NIL")
    return "(%s NIL %s NIL NIL)" % (fields, disposition)


def section_body(part):
    """A part's body as the message keeps it (still in its transfer encoding)."""
    payload = part.get_payload(decode=False)
    if isinstance(payload, str):
        return payload.encode("ascii", "surrogateescape")
    return b""


def section_bytes(raw, section):
    """BODY[section] of a message: HEADER, "" for the whole, or a part's number ("2", "1.2")."""
    if section == "":
        return raw
    head, _, body = raw.partition(b"\r\n\r\n")
    if section.upper() == "HEADER":
        return head + b"\r\n\r\n"
    message = email.message_from_bytes(raw, policy=email.policy.compat32)
    part = message
    for number in section.split("."):
        if part.is_multipart():
            part = part.get_payload()[int(number) - 1]
        elif number != "1":
            return b""
    if not part.is_multipart() and part is message:
        return body
    return section_body(part)


class Store:
    """The mailboxes: name -> list of [uid, flags(set), bytes]; next UID per mailbox."""

    def __init__(self):
        self.lock = threading.Lock()
        self.boxes = {name: [] for name, _ in FOLDERS}
        self.next_uid = {name: 1 for name, _ in FOLDERS}
        for raw in INBOX:
            self.add("INBOX", raw, set())
        self.boxes["INBOX"][2][1].add("\\Seen")

    def add(self, box, raw, flags):
        uid = self.next_uid[box] + 9
        self.next_uid[box] = uid + 1
        self.boxes[box].append([uid, set(flags), raw])
        return uid


STORE = Store()
COUNTERS = {"smtp": 0, "append": 0, "arrivals": 0}
ARRIVALS_MAX = None
OUTDIR = "."


class Lines:
    def __init__(self, sock):
        self.sock = sock
        self.buffer = b""

    def wrap(self, context):
        self.sock = context.wrap_socket(self.sock, server_side=True)
        self.buffer = b""

    def line(self):
        while b"\r\n" not in self.buffer:
            data = self.sock.recv(4096)
            if not data:
                return None
            self.buffer += data
        line, self.buffer = self.buffer.split(b"\r\n", 1)
        return line

    def exact(self, count):
        while len(self.buffer) < count:
            data = self.sock.recv(4096)
            if not data:
                return None
            self.buffer += data
        data, self.buffer = self.buffer[:count], self.buffer[count:]
        return data

    def send(self, data):
        self.sock.sendall(data)


def quoted_args(text):
    """Splits IMAP arguments: quoted strings (with escapes) and atoms."""
    out = []
    i = 0
    while i < len(text):
        if text[i] == " ":
            i += 1
            continue
        if text[i] == '"':
            i += 1
            value = ""
            while i < len(text) and text[i] != '"':
                if text[i] == "\\":
                    i += 1
                value += text[i]
                i += 1
            i += 1
            out.append(value)
        else:
            j = text.find(" ", i)
            if j < 0:
                j = len(text)
            out.append(text[i:j])
            i = j
    return out


def seq_range(spec, top):
    first, _, last = spec.partition(":")
    first = top if first == "*" else int(first)
    last = first if not last else (top if last == "*" else int(last))
    if first > last:
        first, last = last, first
    return first, last


def imap_session(conn, context, secure):
    io = Lines(conn)
    if secure:
        io.wrap(context)
    io.send(b"* OK fake IMAP ready\r\n")
    selected = None
    while True:
        line = io.line()
        if line is None:
            return
        text = line.decode("utf-8", "replace")
        tag, _, rest = text.partition(" ")
        command, _, args = rest.partition(" ")
        command = command.upper()
        if command == "UID":
            sub, _, args = args.partition(" ")
            command = "UID " + sub.upper()
        if command == "CAPABILITY":
            caps = "IMAP4rev1 IDLE STARTTLS" + (" " + EXTRA_CAPS if EXTRA_CAPS else "")
            io.send(("* CAPABILITY %s\r\n" % caps).encode() + tag.encode() + b" OK done\r\n")
        elif command == "STARTTLS":
            io.send(tag.encode() + b" OK begin TLS\r\n")
            io.wrap(context)
        elif command == "LOGIN":
            user, password = quoted_args(args)[:2]
            if user == USER and password == PASSWORD:
                io.send(tag.encode() + b" OK logged in\r\n")
            else:
                io.send(tag.encode() + b" NO [AUTHENTICATIONFAILED] wrong password\r\n")
        elif command == "LIST":
            out = b""
            for name, use in FOLDERS:
                attrs = "\\HasNoChildren" + (" " + use if use else "")
                if JAPANESE and name in JAPANESE_NAMES:
                    attrs = "\\HasNoChildren"
                    name = JAPANESE_NAMES[name]
                    if name == JAPANESE_NAMES["Trash"]:
                        out += ('* LIST (%s) "/" {%d}\r\n%s\r\n' % (attrs, len(name), name)).encode()
                        continue
                out += ('* LIST (%s) "/" "%s"\r\n' % (attrs, name)).encode()
            if JAPANESE:
                out += b"* LIST (\\Noselect \\HasChildren) \"/\" \"Sent Items\"\r\n"
            io.send(out + tag.encode() + b" OK listed\r\n")
        elif command == "SELECT":
            selected = box_name(quoted_args(args)[0])
            with STORE.lock:
                count = len(STORE.boxes[selected])
            io.send(("* %d EXISTS\r\n* 0 RECENT\r\n" % count).encode() + tag.encode() + b" OK [READ-WRITE] selected\r\n")
        elif command in ("FETCH", "UID FETCH"):
            spec, _, items = args.partition(" ")
            with STORE.lock:
                box = list(STORE.boxes[selected])
            out = b""
            for seq, (uid, flags, raw) in enumerate(box, 1):
                if command == "FETCH":
                    first, last = seq_range(spec, len(box))
                    if not first <= seq <= last:
                        continue
                else:
                    first, last = seq_range(spec, box[-1][0] if box else 0)
                    if not (first <= uid <= last or (spec.endswith(":*") and uid == box[-1][0])):
                        continue
                upper = items.upper()
                if "BODYSTRUCTURE" in upper:
                    structure = body_structure(email.message_from_bytes(raw, policy=email.policy.compat32))
                    out += ("* %d FETCH (UID %d BODYSTRUCTURE %s)\r\n" % (seq, uid, structure)).encode()
                    continue
                start = upper.find("BODY.PEEK[")
                end = upper.find("]", start)
                section = items[start + 10:end] if start >= 0 else ""
                data = section_bytes(raw, section)
                partial = items[end + 1:].split(")")[0] if start >= 0 else ""
                if partial.startswith("<0.") and partial.endswith(">"):
                    data = data[:int(partial[3:-1])]
                if section:
                    out += ("* %d FETCH (UID %d BODY[%s]<0> {%d}\r\n" % (seq, uid, section, len(data))).encode()
                else:
                    out += ("* %d FETCH (UID %d FLAGS (%s) RFC822.SIZE %d BODY[]<0> {%d}\r\n"
                            % (seq, uid, " ".join(sorted(flags)), len(raw), len(data))).encode()
                out += data + b")\r\n"
            io.send(out + tag.encode() + b" OK fetched\r\n")
        elif command == "UID STORE":
            uid_text, mode, flag_list = args.split(" ", 2)
            flags = flag_list.strip("()").split()
            with STORE.lock:
                for entry in STORE.boxes[selected]:
                    if entry[0] == int(uid_text):
                        if mode.startswith("+"):
                            entry[1].update(flags)
                        else:
                            entry[1].difference_update(flags)
            io.send(tag.encode() + b" OK stored\r\n")
        elif command == "UID MOVE":
            uid_text, destination = args.split(" ", 1)
            destination = box_name(quoted_args(destination)[0])
            out = b""
            with STORE.lock:
                box = STORE.boxes[selected]
                for seq, entry in enumerate(box, 1):
                    if entry[0] == int(uid_text):
                        STORE.add(destination, entry[2], entry[1])
                        del box[seq - 1]
                        out += ("* %d EXPUNGE\r\n" % seq).encode()
                        break
            io.send(out + tag.encode() + b" OK moved\r\n")
        elif command == "UID EXPUNGE":
            out = b""
            with STORE.lock:
                box = STORE.boxes[selected]
                for seq, entry in enumerate(box, 1):
                    if entry[0] == int(args) and "\\Deleted" in entry[1]:
                        del box[seq - 1]
                        out += ("* %d EXPUNGE\r\n" % seq).encode()
                        break
            io.send(out + tag.encode() + b" OK expunged\r\n")
        elif command == "UID COPY":
            uid_text, destination = args.split(" ", 1)
            destination = box_name(quoted_args(destination)[0])
            with STORE.lock:
                for entry in STORE.boxes[selected]:
                    if entry[0] == int(uid_text):
                        STORE.add(destination, entry[2], entry[1])
            io.send(tag.encode() + b" OK copied\r\n")
        elif command == "EXPUNGE":
            out = b""
            with STORE.lock:
                box = STORE.boxes[selected]
                seq = 1
                while seq <= len(box):
                    if "\\Deleted" in box[seq - 1][1]:
                        del box[seq - 1]
                        out += ("* %d EXPUNGE\r\n" % seq).encode()
                    else:
                        seq += 1
            io.send(out + tag.encode() + b" OK expunged\r\n")
        elif command == "APPEND":
            parts = quoted_args(args)
            mailbox = box_name(parts[0])
            size = int(args[args.rindex("{") + 1:-1])
            io.send(b"+ go ahead\r\n")
            raw = io.exact(size)
            io.line()
            with STORE.lock:
                STORE.add(mailbox, raw, {"\\Seen"})
                COUNTERS["append"] += 1
                number = COUNTERS["append"]
            with open(os.path.join(OUTDIR, "append-%d.eml" % number), "wb") as handle:
                handle.write(raw)
            io.send(tag.encode() + b" OK appended\r\n")
        elif command == "IDLE":
            io.send(b"+ idling\r\n")
            time.sleep(0.5)
            with STORE.lock:
                arrive = ARRIVALS_MAX is None or COUNTERS["arrivals"] < ARRIVALS_MAX
                if arrive:
                    COUNTERS["arrivals"] += 1
                    STORE.add("INBOX", NEW_MESSAGE, set())
                count = len(STORE.boxes["INBOX"])
            if arrive:
                io.send(("* %d EXISTS\r\n" % count).encode())
            done = io.line()
            if done is None:
                return
            io.send(tag.encode() + b" OK idle done\r\n")
        elif command == "LOGOUT":
            io.send(b"* BYE\r\n" + tag.encode() + b" OK bye\r\n")
            return
        else:
            io.send(tag.encode() + b" BAD unknown\r\n")


def smtp_session(conn, context, secure):
    io = Lines(conn)
    if secure:
        io.wrap(context)
    io.send(b"220 fake SMTP ready\r\n")
    tls = secure
    authed = False
    envelope = {"from": None, "to": []}
    while True:
        line = io.line()
        if line is None:
            return
        text = line.decode("utf-8", "replace")
        verb = text.split(" ", 1)[0].upper()
        if verb == "EHLO":
            out = b"250-fake\r\n"
            if not tls:
                out += b"250-STARTTLS\r\n"
            out += b"250-8BITMIME\r\n250 AUTH " + SMTP_AUTH.encode() + b"\r\n"
            io.send(out)
        elif verb == "STARTTLS":
            io.send(b"220 go ahead\r\n")
            io.wrap(context)
            tls = True
        elif verb == "AUTH":
            if not tls:
                io.send(b"530 TLS first\r\n")
                continue
            if text.split(" ")[1].upper() == "LOGIN":
                io.send(b"334 VXNlcm5hbWU6\r\n")
                user = base64.b64decode(io.line() or b"")
                io.send(b"334 UGFzc3dvcmQ6\r\n")
                password = base64.b64decode(io.line() or b"")
                LOGINS.append("LOGIN")
            else:
                blob = base64.b64decode(text.split(" ", 2)[2])
                _, user, password = blob.split(b"\0")
                LOGINS.append("PLAIN")
            with open(os.path.join(OUTDIR, "logins"), "a") as handle:
                handle.write(LOGINS[-1] + "\n")
            if user.decode() == USER and password.decode() == PASSWORD:
                authed = True
                io.send(b"235 ok\r\n")
            else:
                io.send(b"535 bad credentials\r\n")
        elif verb == "MAIL":
            if not authed:
                io.send(b"530 auth first\r\n")
                continue
            envelope["from"] = text[text.index("<") + 1:text.index(">")]
            io.send(b"250 ok\r\n")
        elif verb == "RCPT":
            envelope["to"].append(text[text.index("<") + 1:text.index(">")])
            io.send(b"250 ok\r\n")
        elif verb == "DATA":
            io.send(b"354 go\r\n")
            lines = []
            while True:
                data_line = io.line()
                if data_line is None:
                    return
                if data_line == b".":
                    break
                if data_line.startswith(b"."):
                    data_line = data_line[1:]
                lines.append(data_line)
            with STORE.lock:
                COUNTERS["smtp"] += 1
                number = COUNTERS["smtp"]
            with open(os.path.join(OUTDIR, "smtp-%d.eml" % number), "wb") as handle:
                handle.write(b"\r\n".join(lines) + b"\r\n")
            with open(os.path.join(OUTDIR, "smtp-%d.env" % number), "w") as handle:
                handle.write("from=%s\nto=%s\n" % (envelope["from"], ",".join(envelope["to"])))
            envelope = {"from": None, "to": []}
            io.send(b"250 queued\r\n")
        elif verb == "QUIT":
            io.send(b"221 bye\r\n")
            return
        else:
            io.send(b"502 unknown\r\n")


def serve(listener, handler, context, secure):
    while True:
        conn, _ = listener.accept()
        threading.Thread(target=run_session, args=(handler, conn, context, secure), daemon=True).start()


def run_session(handler, conn, context, secure):
    try:
        handler(conn, context, secure)
    except (ssl.SSLError, OSError):
        pass
    finally:
        conn.close()


def main():
    global OUTDIR, ARRIVALS_MAX, EXTRA_CAPS, JAPANESE, SMTP_AUTH
    cert, key, OUTDIR = sys.argv[1:4]
    bind = "127.0.0.1"
    options = sys.argv[4:]
    while options:
        name = options.pop(0)
        if name == "--bind":
            bind = options.pop(0)
        elif name == "--arrivals":
            ARRIVALS_MAX = int(options.pop(0))
        elif name == "--caps":
            EXTRA_CAPS = options.pop(0)
        elif name == "--large":
            STORE.add("INBOX", large_message(), set())
        elif name == "--smtp-auth":
            SMTP_AUTH = options.pop(0)
        elif name == "--japanese-folders":
            JAPANESE = True
        else:
            sys.exit("fake-mail-server.py: unknown option " + name)
    context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    context.load_cert_chain(cert, key)
    ports = []
    for handler, secure in ((imap_session, True), (imap_session, False), (smtp_session, True), (smtp_session, False)):
        listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        listener.bind((bind, 0))
        listener.listen(8)
        ports.append(listener.getsockname()[1])
        threading.Thread(target=serve, args=(listener, handler, context, secure), daemon=True).start()
    print("PORTS %d %d %d %d" % tuple(ports), flush=True)
    while True:
        time.sleep(3600)


if __name__ == "__main__":
    main()
