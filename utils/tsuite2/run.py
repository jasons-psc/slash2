import logging, sys

from colorama import init, Fore
from argparse import ArgumentParser
from ConfigParser import ConfigParser

from tsuite import TSuite, check_subset

log = logging.getLogger("slash2")

def main():
  """Entry point into the SLASH2 Test Suite.
  Deals with argument parsing and main configuration."""

  #Reset colorama after prints
  init(autoreset=True)

  parser = ArgumentParser(description="SLASH2 Test Suite")
  parser.add_argument("-v", "--verbose", action="count",
    help="increase verbosity", default=0)
  parser.add_argument("-l", "--log-file", help="log output to a file",
    default=None)
  parser.add_argument("-c", "--config-file",
    help="path to slash2 test suite config",
    default="tsuite.conf")
  parser.add_argument("-b", "--build", choices=["src", "svn"],
    help="build from src or svn", default="src")

  args = parser.parse_args()

  #Get log level
  level = [logging.WARNING, logging.INFO, logging.DEBUG]\
      [2 if args.verbose > 2 else args.verbose]

  log.setLevel(level)

  #Setup stream log (console)
  fmt_string = "{2}%(asctime)s{0} [{1}%(levelname)s{0}] {2}%(message)s"\
    .format(Fore.RESET, Fore.CYAN, Fore.WHITE)

  ch = logging.StreamHandler()
  ch.setLevel(level)
  ch.setFormatter(logging.Formatter(fmt_string))

  log.addHandler(ch)

  #Setup file log
  if args.log_file:
    fch = logging.FileHandler(args.log_file)
    fch.setLevel(level)
    fch.setFormatter(
      logging.Formatter("%(asctime)s [%(levelname)s] %(message)s")
    )
    log.addHandler(fch)

  #Check for config file
  conf = ConfigParser()

  if len(conf.read(args.config_file)) == 0:
    log.fatal("Unable to read configuration file!")
    sys.exit(1)

  #Required sections; check for their existence

  sections = {
    "tsuite": [
      "rootdir",
      "logbase"
    ],
    "slash2": [
      "conf",
      "mds_gdb", "ion_gdb", "mnt_gdb"
    ],
    "tests": [
      "testdir",
      "excluded"
    ]
  }

  #Building from source or svn

  if args.build == "svn":
    sections["svn"] = [
      "svnroot"
    ]
  else:
    sections["source"] = [
      "srcroot"
    ]

  #Check that the required sections exist
  missing = check_subset(list(sections), list(conf._sections))
  if len(missing) != 0:
    log.fatal("Configuration file is missing sections!")
    log.fatal("Missing: {}".format(", ".join(missing)))
    sys.exit(1)

  #Check that all the fields listed are present
  #in each section
  for section in sections:
    missing = check_subset(
      sections[section],
      conf._sections[section]
    )
    if len(missing) != 0:
      log.fatal("Missing fields in {} section!".format(section))
      log.fatal("Missing: {}".format(", ".join(missing)))
      sys.exit(1)

  if "timeout" not in conf._sections["slash2"]:
    conf._sections["slash2"]["timeout"] = None

  log.info("Configuration file loaded successfully!")

  #Initialize the test suite
  t = TSuite(conf._sections)
  t.build_mds()
  t.launch_mds()
  t.build_ion()
  t.launch_ion()


if __name__=="__main__":
  main()
