# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this file,
# You can obtain one at http://mozilla.org/MPL/2.0/.

from setuptools import setup

PACKAGE_VERSION = '0.26'

deps = ['mozinfo',
        'mozlog >= 3.0',
        ]

setup(name='moznetwork',
      version=PACKAGE_VERSION,
      description="Library of network utilities for use in Mozilla testing",
      long_description="see http://mozbase.readthedocs.org/",
      classifiers=[], # Get strings from http://pypi.python.org/pypi?%3Aaction=list_classifiers
      keywords='mozilla',
      author='Mozilla Automation and Tools team',
      author_email='tools@lists.mozilla.org',
      url='https://wiki.mozilla.org/Auto-tools/Projects/Mozbase',
      license='MPL',
      packages=['moznetwork'],
      include_package_data=True,
      zip_safe=False,
      install_requires=deps,
      entry_points={'console_scripts': [
          'moznetwork = moznetwork:cli']},
      )
