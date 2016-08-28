#!/usr/bin/env python3

"""Find Kconfig symbols that are referenced but not defined."""

# (c) 2014-2016 Valentin Rothberg <valentinrothberg@gmail.com>
# (c) 2014 Stefan Hengelein <stefan.hengelein@fau.de>
#
# Licensed under the terms of the GNU GPL License version 2


import argparse
import difflib
import os
import re
import signal
import subprocess
import sys
from multiprocessing import Pool, cpu_count


# regex expressions
OPERATORS = r"&|\(|\)|\||\!"
FEATURE = r"(?:\w*[A-Z0-9]\w*){2,}"
DEF = r"^\s*(?:menu){,1}config\s+(" + FEATURE + r")\s*"
EXPR = r"(?:" + OPERATORS + r"|\s|" + FEATURE + r")+"
DEFAULT = r"default\s+.*?(?:if\s.+){,1}"
STMT = r"^\s*(?:if|select|depends\s+on|(?:" + DEFAULT + r"))\s+" + EXPR
SOURCE_FEATURE = r"(?:\W|\b)+[D]{,1}CONFIG_(" + FEATURE + r")"

# regex objects
REGEX_FILE_KCONFIG = re.compile(r".*Kconfig[\.\w+\-]*$")
REGEX_FEATURE = re.compile(r'(?!\B)' + FEATURE + r'(?!\B)')
REGEX_SOURCE_FEATURE = re.compile(SOURCE_FEATURE)
REGEX_KCONFIG_DEF = re.compile(DEF)
REGEX_KCONFIG_EXPR = re.compile(EXPR)
REGEX_KCONFIG_STMT = re.compile(STMT)
REGEX_KCONFIG_HELP = re.compile(r"^\s+(help|---help---)\s*$")
REGEX_FILTER_FEATURES = re.compile(r"[A-Za-z0-9]$")
REGEX_NUMERIC = re.compile(r"0[xX][0-9a-fA-F]+|[0-9]+")
REGEX_QUOTES = re.compile("(\"(.*?)\")")


def parse_options():
    """The user interface of this module."""
    usage = "Run this tool to detect Kconfig symbols that are referenced but " \
            "not defined in Kconfig.  If no option is specified, "             \
            "checkkconfigsymbols defaults to check your current tree.  "       \
            "Please note that specifying commits will 'git reset --hard\' "    \
            "your current tree!  You may save uncommitted changes to avoid "   \
            "losing data."

    parser = argparse.ArgumentParser(description=usage)

    parser.add_argument('-c', '--commit', dest='commit', action='store',
                        default="",
                        help="check if the specified commit (hash) introduces "
                             "undefined Kconfig symbols")

    parser.add_argument('-d', '--diff', dest='diff', action='store',
                        default="",
                        help="diff undefined symbols between two commits "
                             "(e.g., -d commmit1..commit2)")

    parser.add_argument('-f', '--find', dest='find', action='store_true',
                        default=False,
                        help="find and show commits that may cause symbols to be "
                             "missing (required to run with --diff)")

    parser.add_argument('-i', '--ignore', dest='ignore', action='store',
                        default="",
                        help="ignore files matching this Python regex "
                             "(e.g., -i '.*defconfig')")

    parser.add_argument('-s', '--sim', dest='sim', action='store', default="",
                        help="print a list of max. 10 string-similar symbols")

    parser.add_argument('--force', dest='force', action='store_true',
                        default=False,
                        help="reset current Git tree even when it's dirty")

    parser.add_argument('--no-color', dest='color', action='store_false',
                        default=True,
                        help="don't print colored output (default when not "
                             "outputting to a terminal)")

    args = parser.parse_args()

    if args.commit and args.diff:
        sys.exit("Please specify only one option at once.")

    if args.diff and not re.match(r"^[\w\-\.]+\.\.[\w\-\.]+$", args.diff):
        sys.exit("Please specify valid input in the following format: "
                 "\'commit1..commit2\'")

    if args.commit or args.diff:
        if not args.force and tree_is_dirty():
            sys.exit("The current Git tree is dirty (see 'git status').  "
                     "Running this script may\ndelete important data since it "
                     "calls 'git reset --hard' for some performance\nreasons. "
                     " Please run this script in a clean Git tree or pass "
                     "'--force' if you\nwant to ignore this warning and "
                     "continue.")

    if args.commit:
        args.find = False

    if args.ignore:
        try:
            re.match(args.ignore, "this/is/just/a/test.c")
        except:
            sys.exit("Please specify a valid Python regex.")

    return args


def main():
    """Main function of this module."""
    args = parse_options()

    global COLOR
    COLOR = args.color and sys.stdout.isatty()

    if args.sim and not args.commit and not args.diff:
        sims = find_sims(args.sim, args.ignore)
        if sims:
            print("%s: %s" % (yel("Similar symbols"), ', '.join(sims)))
        else:
            print("%s: no similar symbols found" % yel("Similar symbols"))
        sys.exit(0)

    # dictionary of (un)defined symbols
    defined = {}
    undefined = {}

    if args.commit or args.diff:
        head = get_head()

        # get commit range
        commit_a = None
        commit_b = None
        if args.commit:
            commit_a = args.commit + "~"
            commit_b = args.commit
        elif args.diff:
            split = args.diff.split("..")
            commit_a = split[0]
            commit_b = split[1]
            undefined_a = {}
            undefined_b = {}

        # get undefined items before the commit
        execute("git reset --hard %s" % commit_a)
        undefined_a, _ = check_symbols(args.ignore)

        # get undefined items for the commit
        execute("git reset --hard %s" % commit_b)
        undefined_b, defined = check_symbols(args.ignore)

        # report cases that are present for the commit but not before
        for feature in sorted(undefined_b):
            # feature has not been undefined before
            if feature not in undefined_a:
                files = sorted(undefined_b.get(feature))
                undefined[feature] = files
            # check if there are new files that reference the undefined feature
            else:
                files = sorted(undefined_b.get(feature) -
                               undefined_a.get(feature))
                if files:
                    undefined[feature] = files

        # reset to head
        execute("git reset --hard %s" % head)

    # default to check the entire tree
    else:
        undefined, defined = check_symbols(args.ignore)

    # now print the output
    for feature in sorted(undefined):
        print(red(feature))

        files = sorted(undefined.get(feature))
        print("%s: %s" % (yel("Referencing files"), ", ".join(files)))

        sims = find_sims(feature, args.ignore, defined)
        sims_out = yel("Similar symbols")
        if sims:
            print("%s: %s" % (sims_out, ', '.join(sims)))
        else:
            print("%s: %s" % (sims_out, "no similar symbols found"))

        if args.find:
            print("%s:" % yel("Commits changing symbol"))
            commits = find_commits(feature, args.diff)
            if commits:
                for commit in commits:
                    commit = commit.split(" ", 1)
                    print("\t- %s (\"%s\")" % (yel(commit[0]), commit[1]))
            else:
                print("\t- no commit found")
        print()  # new line


def yel(string):
    """
    Color %string yellow.
    """
    return "\033[33m%s\033[0m" % string if COLOR else string


def red(string):
    """
    Color %string red.
    """
    return "\033[31m%s\033[0m" % string if COLOR else string


def execute(cmd):
    """Execute %cmd and return stdout.  Exit in case of error."""
    try:
        cmdlist = cmd.split(" ")
        stdout = subprocess.check_output(cmdlist, stderr=subprocess.STDOUT, shell=False)
        stdout = stdout.decode(errors='replace')
    except subprocess.CalledProcessError as fail:
        exit("Failed to execute %s\n%s" % (cmd, fail))
    return stdout


def find_commits(symbol, diff):
    """Find commits changing %symbol in the given range of %diff."""
    commits = execute("git log --pretty=oneline --abbrev-commit -G %s %s"
                      % (symbol, diff))
    return [x for x in commits.split("\n") if x]


def tree_is_dirty():
    """Return true if the current working tree is dirty (i.e., if any file has
    been added, deleted, modified, renamed or copied but not committed)."""
    stdout = execute("git status --porcelain")
    for line in stdout:
        if re.findall(r"[URMADC]{1}", line[:2]):
            return True
    return False


def get_head():
    """Return commit hash of current HEAD."""
    stdout = execute("git rev-parse HEAD")
    return stdout.strip('\n')


def partition(lst, size):
    """Partition list @lst into eveni-sized lists of size @size."""
    return [lst[i::size] for i in range(size)]


def init_worker():
    """Set signal handler to ignore SIGINT."""
    signal.signal(signal.SIGINT, signal.SIG_IGN)


def find_sims(symbol, ignore, defined=[]):
    """Return a list of max. ten Kconfig symbols that are string-similar to
    @symbol."""
    if defined:
        return sorted(difflib.get_close_matches(symbol, set(defined), 10))

    pool = Pool(cpu_count(), init_worker)
    kfiles = []
    for gitfile in get_files():
        if REGEX_FILE_KCONFIG.match(gitfile):
            kfiles.append(gitfile)

    arglist = []
    for part in partition(kfiles, cpu_count()):
        arglist.append((part, ignore))

    for res in pool.map(parse_kconfig_files, arglist):
        defined.extend(res[0])

    return sorted(difflib.get_close_matches(symbol, set(defined), 10))


def get_files():
    """Return a list of all files in the current git directory."""
    # use 'git ls-files' to get the worklist
    stdout = execute("git ls-files")
    if len(stdout) > 0 and stdout[-1] == "\n":
        stdout = stdout[:-1]

    files = []
    for gitfile in stdout.rsplit("\n"):
        if ".git" in gitfile or "ChangeLog" in gitfile or      \
                ".log" in gitfile or os.path.isdir(gitfile) or \
                gitfile.startswith("tools/"):
            continue
        files.append(gitfile)
    return files


def check_symbols(ignore):
    """Find undefined Kconfig symbols and return a dict with the symbol as key
    and a list of referencing files as value.  Files matching %ignore are not
    checked for undefined symbols."""
    pool = Pool(cpu_count(), init_worker)
    try:
        return check_symbols_helper(pool, ignore)
    except KeyboardInterrupt:
        pool.terminate()
        pool.join()
        sys.exit(1)


def check_symbols_helper(pool, ignore):
    """Helper method for check_symbols().  Used to catch keyboard interrupts in
    check_symbols() in order to properly terminate running worker processes."""
    source_files = []
    kconfig_files = []
    defined_features = []
    referenced_features = dict()  # {file: [features]}

    for gitfile in get_files():
        if REGEX_FILE_KCONFIG.match(gitfile):
            kconfig_files.append(gitfile)
        else:
            if ignore and not re.match(ignore, gitfile):
                continue
            # add source files that do not match the ignore pattern
            source_files.append(gitfile)

    # parse source files
    arglist = partition(source_files, cpu_count())
    for res in pool.map(parse_source_files, arglist):
        referenced_features.update(res)

    # parse kconfig files
    arglist = []
    for part in partition(kconfig_files, cpu_count()):
        arglist.append((part, ignore))
    for res in pool.map(parse_kconfig_files, arglist):
        defined_features.extend(res[0])
        referenced_features.update(res[1])
    defined_features = set(defined_features)

    # inverse mapping of referenced_features to dict(feature: [files])
    inv_map = dict()
    for _file, features in referenced_features.items():
        for feature in features:
            inv_map[feature] = inv_map.get(feature, set())
            inv_map[feature].add(_file)
    referenced_features = inv_map

    undefined = {}  # {feature: [files]}
    for feature in sorted(referenced_features):
        # filter some false positives
        if feature == "FOO" or feature == "BAR" or \
                feature == "FOO_BAR" or feature == "XXX":
            continue
        if feature not in defined_features:
            if feature.endswith("_MODULE"):
                # avoid false positives for kernel modules
                if feature[:-len("_MODULE")] in defined_features:
                    continue
            undefined[feature] = referenced_features.get(feature)
    return undefined, defined_features


def parse_source_files(source_files):
    """Parse each source file in @source_files and return dictionary with source
    files as keys and lists of references Kconfig symbols as values."""
    referenced_features = dict()
    for sfile in source_files:
        referenced_features[sfile] = parse_source_file(sfile)
    return referenced_features


def parse_source_file(sfile):
    """Parse @sfile and return a list of referenced Kconfig features."""
    lines = []
    references = []

    if not os.path.exists(sfile):
        return references

    with open(sfile, "r", encoding='utf-8', errors='replace') as stream:
        lines = stream.readlines()

    for line in lines:
        if "CONFIG_" not in line:
            continue
        features = REGEX_SOURCE_FEATURE.findall(line)
        for feature in features:
            if not REGEX_FILTER_FEATURES.search(feature):
                continue
            references.append(feature)

    return references


def get_features_in_line(line):
    """Return mentioned Kconfig features in @line."""
    return REGEX_FEATURE.findall(line)


def parse_kconfig_files(args):
    """Parse kconfig files and return tuple of defined and references Kconfig
    symbols.  Note, @args is a tuple of a list of files and the @ignore
    pattern."""
    kconfig_files = args[0]
    ignore = args[1]
    defined_features = []
    referenced_features = dict()

    for kfile in kconfig_files:
        defined, references = parse_kconfig_file(kfile)
        defined_features.extend(defined)
        if ignore and re.match(ignore, kfile):
            # do not collect references for files that match the ignore pattern
            continue
        referenced_features[kfile] = references
    return (defined_features, referenced_features)


def parse_kconfig_file(kfile):
    """Parse @kfile and update feature definitions and references."""
    lines = []
    defined = []
    references = []
    skip = False

    if not os.path.exists(kfile):
        return defined, references

    with open(kfile, "r", encoding='utf-8', errors='replace') as stream:
        lines = stream.readlines()

    for i in range(len(lines)):
        line = lines[i]
        line = line.strip('\n')
        line = line.split("#")[0]  # ignore comments

        if REGEX_KCONFIG_DEF.match(line):
            feature_def = REGEX_KCONFIG_DEF.findall(line)
            defined.append(feature_def[0])
            skip = False
        elif REGEX_KCONFIG_HELP.match(line):
            skip = True
        elif skip:
            # ignore content of help messages
            pass
        elif REGEX_KCONFIG_STMT.match(line):
            line = REGEX_QUOTES.sub("", line)
            features = get_features_in_line(line)
            # multi-line statements
            while line.endswith("\\"):
                i += 1
                line = lines[i]
                line = line.strip('\n')
                features.extend(get_features_in_line(line))
            for feature in set(features):
                if REGEX_NUMERIC.match(feature):
                    # ignore numeric values
                    continue
                references.append(feature)

    return defined, references


if __name__ == "__main__":
    main()
