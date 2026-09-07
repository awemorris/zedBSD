/* Host copy of the generic inode kind conversion; native tests use the real function. */
mode_t
inode_type_mode(
	enum inode_type type)
{
	switch (type) {
	case INODE_REG:
		return S_IFREG;
	case INODE_DIR:
		return S_IFDIR;
	case INODE_BLOCK:
		return S_IFBLK;
	case INODE_CHAR:
		return S_IFCHR;
	case INODE_SYMLINK:
		return S_IFLNK;
	case INODE_SOCKET:
		return S_IFSOCK;
	case INODE_FIFO:
		return S_IFIFO;
	default:
		return 0;
	}
}

