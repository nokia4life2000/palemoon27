# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at http://mozilla.org/MPL/2.0/.

from __future__ import absolute_import, unicode_literals, print_function

from mozbuild.backend.common import CommonBackend
from mozbuild.frontend.data import (
    ContextDerived,
    Defines,
    DistFiles,
    FinalTargetFiles,
    JARManifest,
    JavaScriptModules,
    JsPreferenceFile,
    Resources,
    VariablePassthru,
)
from mozbuild.jar import JarManifestParser
from mozbuild.makeutil import Makefile
from mozbuild.preprocessor import Preprocessor
from mozbuild.util import OrderedDefaultDict
from mozpack.manifests import InstallManifest
import mozpack.path as mozpath
from collections import OrderedDict
from itertools import chain
import os
import sys


class OverwriteInstallManifest(InstallManifest):
    def _add_entry(self, dest, entry):
        # Because of bug 1210703, we can't let the default behavior of
        # InstallManifest._add_entry, which is to error out.
        # To match the current behavior of the recursive make libs tier, we
        # keep the last one given, but still warn about it.
        if dest in self._dests:
            print('Warning: Item already in manifest: %s' % dest,
                  file=sys.stderr)

        self._dests[dest] = entry


class FasterMakeBackend(CommonBackend):
    def _init(self):
        super(FasterMakeBackend, self)._init()

        self._seen_directories = set()
        self._defines = dict()

        self._manifest_entries = OrderedDefaultDict(list)

        self._install_manifests = OrderedDefaultDict(OverwriteInstallManifest)

        self._dependencies = OrderedDefaultDict(list)

    def _add_preprocess(self, obj, path, dest, target=None, **kwargs):
        if target is None:
            target = mozpath.basename(path)
            # This matches what PP_TARGETS do in config/rules.
            if target.endswith('.in'):
                target = target[:-3]
        depfile = mozpath.join(
            self.environment.topobjdir, 'faster', '.deps',
            mozpath.join(obj.install_target, dest, target).replace('/', '_'))
        self._install_manifests[obj.install_target].add_preprocess(
            mozpath.join(obj.srcdir, path),
            mozpath.join(dest, target),
            depfile,
            **kwargs)

    def consume_object(self, obj):
        if not isinstance(obj, Defines) and isinstance(obj, ContextDerived):
            defines = self._defines.get(obj.objdir, {})
            if defines:
                defines = defines.defines

        if isinstance(obj, Defines):
            self._defines[obj.objdir] = obj

            # We're assuming below that Defines come first for a given objdir,
            # which is kind of set in stone from the order things are treated
            # in emitter.py.
            assert obj.objdir not in self._seen_directories

        elif isinstance(obj, JARManifest) and \
                obj.install_target.startswith('dist/bin'):
            self._consume_jar_manifest(obj, defines)

        elif isinstance(obj, VariablePassthru) and \
                obj.install_target.startswith('dist/bin'):
            for f in obj.variables.get('EXTRA_COMPONENTS', {}):
                path = mozpath.join(obj.install_target, 'components',
                                    mozpath.basename(f))
                self._install_manifests[obj.install_target].add_symlink(
                    mozpath.join(obj.srcdir, f),
                    mozpath.join('components', mozpath.basename(f))
                )
                if f.endswith('.manifest'):
                    manifest = mozpath.join(obj.install_target,
                                            'chrome.manifest')
                    self._manifest_entries[manifest].append(
                        'manifest components/%s' % mozpath.basename(f))

            for f in obj.variables.get('EXTRA_PP_COMPONENTS', {}):
                self._add_preprocess(obj, f, 'components', defines=defines)

                if f.endswith('.manifest'):
                    manifest = mozpath.join(obj.install_target,
                                            'chrome.manifest')
                    self._manifest_entries[manifest].append(
                        'manifest components/%s' % mozpath.basename(f))

        elif isinstance(obj, JavaScriptModules) and \
                obj.install_target.startswith('dist/bin'):
            for path, strings in obj.modules.walk():
                base = mozpath.join('modules', path)
                for f in strings:
                    if obj.flavor == 'extra':
                        self._install_manifests[obj.install_target].add_symlink(
                            mozpath.join(obj.srcdir, f),
                            mozpath.join(base, mozpath.basename(f))
                        )
                    elif obj.flavor == 'extra_pp':
                        self._add_preprocess(obj, f, base, defines=defines)

        elif isinstance(obj, JsPreferenceFile) and \
                obj.install_target.startswith('dist/bin'):
            # The condition for the directory value in config/rules.mk is:
            # ifneq (,$(DIST_SUBDIR)$(XPI_NAME))
            # - when XPI_NAME is set, obj.install_target will start with
            # dist/xpi-stage
            # - when DIST_SUBDIR is set, obj.install_target will start with
            # dist/bin/$(DIST_SUBDIR)
            # So an equivalent condition that is not cumbersome for us and that
            # is enough at least for now is checking if obj.install_target is
            # different from dist/bin.
            if obj.install_target == 'dist/bin':
                pref_dir = 'defaults/pref'
            else:
                pref_dir = 'defaults/preferences'

            dest = mozpath.join(obj.install_target, pref_dir,
                                mozpath.basename(obj.path))
            # We preprocess these, but they don't necessarily have preprocessor
            # directives, so tell the preprocessor to not complain about that.
            self._add_preprocess(obj, obj.path, pref_dir, defines=defines,
                                 silence_missing_directive_warnings=True)

        elif isinstance(obj, Resources) and \
                obj.install_target.startswith('dist/bin'):
            for path, strings in obj.resources.walk():
                base = mozpath.join('res', path)
                for f in strings:
                    flags = strings.flags_for(f)
                    if flags and flags.preprocess:
                        self._add_preprocess(obj, f, base, marker='%',
                                             defines=obj.defines)
                    else:
                        self._install_manifests[obj.install_target].add_symlink(
                            mozpath.join(obj.srcdir, f),
                            mozpath.join(base, mozpath.basename(f))
                        )

        elif isinstance(obj, FinalTargetFiles) and \
                obj.install_target.startswith('dist/bin'):
            for path, strings in obj.files.walk():
                base = mozpath.join(obj.install_target, path)
                for f in strings:
                    self._install_manifests[obj.install_target].add_symlink(
                        mozpath.join(obj.srcdir, f),
                        mozpath.join(path, mozpath.basename(f))
                    )

        elif isinstance(obj, DistFiles) and \
                obj.install_target.startswith('dist/bin'):
            # We preprocess these, but they don't necessarily have preprocessor
            # directives, so tell the preprocessor to not complain about that.
            for f in obj.files:
                self._add_preprocess(obj, f, '', defines=defines,
                                     silence_missing_directive_warnings=True)

        else:
            # We currently ignore a lot of object types, so just acknowledge
            # everything.
            return True

        self._seen_directories.add(obj.objdir)
        return True

    def _consume_jar_manifest(self, obj, defines):
        # Ideally, this would all be handled somehow in the emitter, but
        # this would require all the magic surrounding l10n and addons in
        # the recursive make backend to die, which is not going to happen
        # any time soon enough.
        # Notably missing:
        # - DEFINES from config/config.mk
        # - L10n support
        # - The equivalent of -e when USE_EXTENSION_MANIFEST is set in
        #   moz.build, but it doesn't matter in dist/bin.
        pp = Preprocessor()
        pp.context.update(defines)
        pp.context.update(self.environment.defines)
        pp.context.update(
            AB_CD='en-US',
            BUILD_FASTER=1,
        )
        pp.out = JarManifestParser()
        pp.do_include(obj.path)

        for jarinfo in pp.out:
            install_target = obj.install_target
            # Bug 1150417 added some gross hacks, which we don't try to
            # support generically. Fortunately, the hacks don't define more
            # than chrome manifest entries, so just assume we don't get
            # any installation entries.
            if jarinfo.name.startswith('../'):
                assert not jarinfo.entries

            base = mozpath.join('chrome', jarinfo.name)

            for e in jarinfo.entries:
                if e.is_locale:
                    src = mozpath.join(
                        jarinfo.relativesrcdir or mozpath.dirname(obj.path),
                        'en-US',
                        e.source)
                elif e.source.startswith('/'):
                    src = mozpath.join(self.environment.topsrcdir,
                                       e.source[1:])
                else:
                    src = mozpath.join(mozpath.dirname(obj.path), e.source)

                if '*' in e.source:
                    if e.preprocess:
                        raise Exception('%s: Wildcards are not supported with '
                                        'preprocessing' % obj.path)
                    def _prefix(s):
                        for p in s.split('/'):
                            if '*' not in p:
                                yield p + '/'
                    prefix = ''.join(_prefix(src))

                    self._install_manifests[obj.install_target] \
                        .add_pattern_symlink(
                        prefix,
                        src[len(prefix):],
                        mozpath.join(base, e.output))
                    continue

                if not os.path.exists(src):
                    if e.is_locale:
                        raise Exception(
                            '%s: Cannot find %s' % (obj.path, e.source))
                    if e.source.startswith('/'):
                        src = mozpath.join(self.environment.topobjdir,
                                           e.source[1:])
                    else:
                        # This actually gets awkward if the jar.mn is not
                        # in the same directory as the moz.build declaring
                        # it, but it's how it works in the recursive make,
                        # not that anything relies on that, but it's simpler.
                        src = mozpath.join(obj.objdir, e.source)
                    self._dependencies['install-%s' % obj.install_target] \
                        .append(mozpath.relpath(
                        src, self.environment.topobjdir))

                if e.preprocess:
                    kwargs = {}
                    if src.endswith('.css'):
                        kwargs['marker'] = '%'
                    self._add_preprocess(
                        obj,
                        src,
                        mozpath.join(base, mozpath.dirname(e.output)),
                        mozpath.basename(e.output),
                        defines=defines,
                        **kwargs)
                else:
                    self._install_manifests[obj.install_target].add_symlink(
                        src,
                        mozpath.join(base, e.output))

            manifest = mozpath.normpath(mozpath.join(obj.install_target, base))
            manifest += '.manifest'
            for m in jarinfo.chrome_manifests:
                self._manifest_entries[manifest].append(
                    m.replace('%', jarinfo.name + '/'))

            # ../ special cased for bug 1150417 again.
            if not jarinfo.name.startswith('../'):
                manifest = mozpath.normpath(mozpath.join(obj.install_target,
                                                         'chrome.manifest'))
                entry = 'manifest %s.manifest' % base
                if entry not in self._manifest_entries[manifest]:
                    self._manifest_entries[manifest].append(entry)

    def consume_finished(self):
        mk = Makefile()
        # Add the default rule at the very beginning.
        mk.create_rule(['default'])
        mk.add_statement('TOPSRCDIR = %s' % self.environment.topsrcdir)
        mk.add_statement('TOPOBJDIR = %s' % self.environment.topobjdir)

        # Add a few necessary variables inherited from configure
        for var in (
            'PYTHON',
            'ACDEFINES',
            'MOZ_BUILD_APP',
            'MOZ_WIDGET_TOOLKIT',
        ):
            mk.add_statement('%s = %s' % (var, self.environment.substs[var]))

        # Add information for chrome manifest generation
        manifest_targets = []

        for target, entries in self._manifest_entries.iteritems():
            manifest_targets.append(target)
            target = '$(TOPOBJDIR)/%s' % target
            mk.create_rule([target]).add_dependencies(
                ['content = %s' % ' '.join('"%s"' % e for e in entries)])

        mk.add_statement('MANIFEST_TARGETS = %s' % ' '.join(manifest_targets))

        # Add information for install manifests.
        mk.add_statement('INSTALL_MANIFESTS = %s'
                         % ' '.join(self._install_manifests.keys()))

        # Add dependencies we infered:
        for target, deps in self._dependencies.iteritems():
            mk.create_rule([target]).add_dependencies(
                '$(TOPOBJDIR)/%s' % d for d in deps)

        mk.add_statement('include $(TOPSRCDIR)/config/faster/rules.mk')

        for base, install_manifest in self._install_manifests.iteritems():
            with self._write_file(
                    mozpath.join(self.environment.topobjdir, 'faster',
                                 'install_%s' % base.replace('/', '_'))) as fh:
                install_manifest.write(fileobj=fh)

        with self._write_file(
                mozpath.join(self.environment.topobjdir, 'faster',
                             'Makefile')) as fh:
            mk.dump(fh, removal_guard=False)
