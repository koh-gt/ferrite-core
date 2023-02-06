Contributing to Ferrite Core
============================

The Ferrite Core project operates an open contributor model where anyone is
welcome to contribute towards development in the form of peer review, testing
and patches. This document explains the practical process and guidelines for
contributing.

Firstly in terms of structure, there is no particular concept of "Core
developers" in the sense of privileged people. Open source often naturally
revolves around meritocracy where longer term contributors gain more trust from
the developer community. However, some hierarchy is necessary for practical
purposes. As such there are repository "maintainers" who are responsible for
merging pull requests as well as a "lead maintainer" who is responsible for the
release cycle, overall merging, moderation and appointment of maintainers.

Communication Channels
----------------------

Most communication about Ferrite Core development happens on the
#ferrite-core channel on Telegram. The easiest way to participate on Telegram is
with the web client, [Telegram](https://t.me/ferrite-core/).

Discussion about code base improvements happens in GitHub issues and on pull
requests.

Contributor Workflow
--------------------

The codebase is maintained using the "contributor workflow" where everyone
without exception contributes patch proposals using "pull requests". This
facilitates social contribution, easy testing and peer review.

To contribute a patch, the workflow is as follows:

  1. Fork repository
  1. Create topic branch
  1. Commit patches

The project coding conventions in the [developer notes](doc/developer-notes.md)
must be adhered to.

In general [commits should be atomic](https://en.wikipedia.org/wiki/Atomic_commit#Atomic_commit_convention)
and diffs should be easy to read. For this reason do not mix any formatting
fixes or code moves with actual code changes.

Commit messages should be verbose by default consisting of a short subject line
(50 chars max), a blank line and detailed explanatory text as separate
paragraph(s), unless the title alone is self-explanatory (like "Corrected typo
in init.cpp") in which case a single title line is sufficient. 

The title of the pull request should be prefixed by the component or area that
the pull request affects. Valid areas as:

  - *Consensus* for changes to consensus critical code
  - *Docs* for changes to the documentation
  - *Qt* for changes to ferrite-qt
  - *Mining* for changes to the mining code
  - *Net* or *P2P* for changes to the peer-to-peer network code
  - *RPC/REST/ZMQ* for changes to the RPC, REST or ZMQ APIs
  - *Scripts and tools* for changes to the scripts and tools
  - *Tests* for changes to the ferrite unit tests or QA tests
  - *Trivial* should **only** be used for PRs that do not change generated
    executable code. Notably, refactors (change of function arguments and code
    reorganization) and changes in behavior should **not** be marked as trivial.
    Examples of trivial PRs are changes to:
    - comments
    - whitespace
    - variable names
    - logging and messages
  - *Utils and libraries* for changes to the utils and libraries
  - *Wallet* for changes to the wallet code

Examples:

    Consensus: Add new opcode for BIP-XXXX OP_CHECKAWESOMESIG
    Net: Automatically create hidden service, listen on Tor
    Qt: Add feed bump button
    Trivial: Fix typo in init.cpp

The body of the pull request should contain enough description about what the
patch does together with any justification/reasoning. You should include
references to any discussions (for example other tickets or mailing list
discussions).

At this stage one should expect comments and review from other contributors. You
can add more commits to your pull request by committing them locally and pushing
to your fork until you have satisfied all feedback.

Squashing Commits
---------------------------
If your pull request is accepted for merging, you may be asked by a maintainer
to squash and or [rebase](https://git-scm.com/docs/git-rebase) your commits
before it will be merged. The basic squashing workflow is shown below.

    git checkout your_branch_name
    git rebase -i HEAD~n
    # n is normally the number of commits in the pull request.
    # Set commits (except the one in the first line) from 'pick' to 'squash', save and quit.
    # On the next screen, edit/refine commit messages.
    # Save and quit.
    git push -f # (force push to GitHub)

Please update the resulting commit message if needed, it should read as a
coherent message. In most cases this means that you should not just list the
interim commits.

If you have problems with squashing (or other workflows with `git`), you can
alternatively enable "Allow edits from maintainers" in the right GitHub
sidebar and ask for help in the pull request.

Pull Request Philosophy
-----------------------

Patchsets should always be focused. For example, a pull request could add a
feature, fix a bug, or refactor code; but not a mixture. Please also avoid super
pull requests which attempt to do too much, are overly large, or overly complex
as this makes review difficult.


### Features

When adding a new feature, thought must be given to the long term technical debt
and maintenance that feature may require after inclusion. Before proposing a new
feature that will require maintenance, please consider if you are willing to
maintain it (including bug fixing). If features get orphaned with no maintainer
in the future, they may be removed by the Repository Maintainer.


### Refactoring

Refactoring is a necessary part of any software project's evolution. The
following guidelines cover refactoring pull requests for the project.

Project maintainers aim for a quick turnaround on refactoring pull requests, so
where possible keep them short, uncomplex and easy to verify.


"Decision Making" Process
-------------------------

The following applies to code changes to the Ferrite Core project.

In general, all pull requests must:

  - Have a clear use case, fix a demonstrable bug or serve the greater good of
    the project (for example refactoring for modularisation);
  - Be well peer reviewed;
  - Where bugs are fixed, where possible, there should be unit tests
    demonstrating the bug and also proving the fix. This helps prevent regression.

Patches that change Ferrite consensus rules are considerably more involved than
normal because they affect the entire ecosystem and so must be preceded by
extensive mailing list discussions and have a numbered BIP. 

While each case will
be different, one should be prepared to expend more time and effort than for
other kinds of patches because of increased peer review and consensus building
requirements.


### Peer Review

Anyone may participate in peer review which is expressed by comments in the pull
request. Typically reviewers will review the code for obvious errors, as well as
test out the patch set and opine on the technical merits of the patch.

Project maintainers reserve the right to weigh the opinions of peer reviewers
using common sense judgement and also may weight based on meritocracy: Those
that have demonstrated a deeper commitment and understanding towards the project
(over time) or have clear domain expertise may naturally have more weight, as
one would expect in all walks of life.

Where a patch set affects consensus critical code, the bar will be set much
higher in terms of discussion and peer review requirements, keeping in mind that
mistakes could be very costly to the wider community. This includes refactoring
of consensus critical code.

Where a patch set proposes to change the Ferrite consensus, it must have been
discussed extensively.

Release Policy
--------------

The project leader is the release manager for each Ferrite Core release.

Copyright
---------

By contributing to this repository, you agree to license your work under the 
MIT license unless specified otherwise in `contrib/debian/copyright` or at 
the top of the file itself. Any work contributed where you are not the original 
author must contain its license header with the original author(s) and source.
