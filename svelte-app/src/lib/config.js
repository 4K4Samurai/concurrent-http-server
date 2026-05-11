export const menus = {
	topbar: [{ path: `/`, label: 'Home' }],
	leftdropdowns: [
		{
			label: 'Public',
			entries: [{ path: `/public`, label: 'Public Content' }]
		}
	],
	rightdropdowns: [
		{
			label: 'Private',
			onlyifauthenticated: true,
			entries: [
				{ path: `/protected`, label: 'Private Content' },
				{ path: `/player`, label: 'Play MP4' }
			]
		}
	]
};

export const branding = 'CS3214 Demo App Spring 2026';
