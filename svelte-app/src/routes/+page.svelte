<script>
	import { version } from '$app/environment';
	import { Container, Row, Col } from '@sveltestrap/sveltestrap';
	const logo = '/images/svelte-logo.svg';
	const cs3214 = '/images/cs3214.png';
</script>

<Container>
	<Row class="mt-4">
		<h1>CS3214 Demo App by <tt>dinmukhamed</tt> and <tt>kgcabanacan</tt></h1>
	</Row>
	<Row>
		<Col xs={{ size: 9 }} md={{ size: 6 }}>
			<img alt="CS3214 logo" src={cs3214} class="cs3214-logo" />
		</Col>
		<Col xs={{ size: 6 }} md={{ size: 6 }}>
			<img alt="Svelte Logo" src={logo} class="app-logo" />
		</Col>
	</Row>
	<Row>
		<Col class="mt-3">
			<p class="lead">
				This small <a href="https://svelte.dev/">Svelte</a>{' '}
				version {version}{' '}
				app shows how to use the JWT authentication facilities of your server in a single-page web application.
			</p>
		</Col>
	</Row>
	<Row>
		<Col>
			Click <a href={`/protected`}>here</a> to navigate to a protected section of the app.
		</Col>
	</Row>
</Container>

<style>
	.app-logo {
		height: 19vmin;
		padding: 1rem;
		width: 100%;
	}
	.cs3214-logo {
		height: 19vmin;
		padding: 1rem;
		width: 100%;
	}
</style>
