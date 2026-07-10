# Developer Branch

This branch is the course-shaped starter plus implemented developer extensions.
It is not a completed lab solution: `sfs_getpos`, `sfs_seek`, `sfs_rename`, and
student synchronization remain deliberately unfinished. Do not merge it into
`main` or publish completed student work here.

## Relationship To Main

| Area | `main` | `developer` |
| --- | --- | --- |
| Scored student task | Three APIs plus synchronization | Unchanged |
| File-size API | Not present | `sfs_fstat`, `sfs_ftruncate` |
| Optional routes | Course-shaped starter behavior | X00, X01, X02 implemented |
| Automatic score | 22 points | Same 22 points; D/X are separate |

Use `main` for a faithful starter exercise. Use `developer` to inspect or extend
the additional filesystem semantics without turning them into score requirements.

## Implemented Extensions

The historical developer TODOs for file-size operations are implemented as:

- `sfs_fstat(fd)`: returns the current file size or `-EBADF`.
- `sfs_ftruncate(fd, length)`: grows with zero-fill or shrinks and releases
  blocks. Allocation failure is transactional. Shrinking clamps every open
  descriptor for the same file to the new end.

Run the unscored extension check with:

```sh
make developer-test
```

The three historical optional challenges are also implemented:

- X00: live empty files consume no data block. Their on-disk `first_block` is
  `SFS_EMPTY_FILE_BLOCK`, a reserved value that cannot be a valid block ID.
- X01: the root directory expands through `next_rootdir` directory blocks.
- X02: removing an open file unlinks its name immediately and reclaims its
  blocks after the final descriptor closes.

Run their unscored checks with:

```sh
make x-traces
```

`sfs-fsck` understands both original one-block empty files and the developer
branch's zero-block encoding. Read, write, and truncate preserve compatibility
with those old images. Its ownership map also uses wide tags, so checking an
expanded directory is not capped at 250 files.

Compatibility is intentionally one-way: developer code accepts original images,
but images containing `SFS_EMPTY_FILE_BLOCK` are not intended for tools from
`main`.

## Deliberate Non-Change

The old note also considered replacing public integer descriptors with a C
newtype. That is intentionally not implemented: it would break every existing
caller without preventing misuse at runtime. The extension functions retain
the established `int fd` ABI.

## Support-Tool Limits

The inherited ASCII escaping assumption and block-device `st_size == 0`
FIXMEs in `sfs-fsck.c` remain out of scope. SFS images are regular files in
this project, and escaping bytes as stable ASCII output is intentional.

## Validation Contract

Before pushing this branch, run:

```sh
make -C sfslab clean all
make developer-test
make x-traces
cd sfslab && ./test-sfs -q
```

The ordinary 22-point score stays separate from Categories D and X. The
incomplete starter still reports `6/22` and exits 1.
