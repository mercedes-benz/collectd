# collectd mbient README

This is the INTERNAL mbient subfolder for collectd.

## 1. Directory layout

The content of this directory is not designated to be upstreamed. Content that is designed to be upstreamed, that is
open-sourced, should go outside this directory. This directory layout assumes most content will be open-sourced and will
otherwise not be efficient.

## 2. Testing

### 2.1 collectd: Unit Tests

Test suite defined in Makefile.am .

### 2.2 mbient: Integration Tests for collectd

Test suite defined in `collectd_test.py` in meta-mbient .

### 2.3 mbient: Smoke Tests for collectd

Test suite defined in https://issue.swf.i.mercedes-benz.com/browse/TMX-82734 .

## 3. Contributing

1. Contributions should be submitted as Gitlab merge requests.

2. Merge requests should include a ticket ID. Commit description should not include a ticket id, as we would like to
keep commits upstreamable.

3. Please refrain from creating or modifying tags without aligning with the Maintainers as it interferes with the release management. (It should go without saying but it did happen.)

A template is available here: https://wiki.swf.i.mercedes-benz.com/display/public/WoW/Template+-+Commit+Message

## 4. Target platforms

1. **Ubuntu-22.04@x86_64**

This is the main platform for component development & component CI pipeline(s).

2. **MBient/Richos-...@{x86_64,binz,adeleg,...}**

This platform is targeted in integration stage by manual testing and in CI.

3. **MBient/QNX**

This platform is targeted in integration stage by manual testing and in CI.

collectd  CIVIC Gen20x.i3 configuration is determined by mbient/CMakeLists.txt

## 5. Release policy

Unreleased features are accumulated in the [ChangeLog](CHANGELOG.md#unreleased). Releases are created on substantial progress or special request.

* * *
This README is based on https://git.swf.daimler.com/aoezmer/repository-readme/-/blob/master/README.template.md .
